/*
   KenRadiant: Play mode - the 3D view as the game shows it, with the map's sounds.

   On: Game View, shaders like in game, the skybox, and the light the game will have (the compiled .bsp when it is
   newer than the map, else the live light preview); target_speaker sounds play around the camera (looping ones,
   timed ones with "wait" / "random", global ones everywhere), attenuated like the engine, and the worldspawn "music".
   Off: everything back as it was.

   Sound: miniaudio (public domain / MIT-0, libs/miniaudio) - WAV (PCM / ADPCM) and MP3 from the game's files and pk3s.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_FLAC
#if defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "miniaudio/miniaudio.h"
#if defined( __GNUC__ )
#pragma GCC diagnostic pop
#endif

#include <vector>
#include <memory>
#include <map>
#include <random>
#include <cmath>
#include <cstring>
#include <QTimer>
#include <QFileInfo>
#include <QDateTime>

#include "ientity.h"
#include "ifilesystem.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "os/path.h"
#include "math/vector.h"
#include "commands.h"
#include "stringio.h"
#include "dialog.h"
#include "gtkutil/widget.h"
#include "map.h"
#include "qerplugin.h"

void KrCamera_getView( Vector3& origin, Vector3& angles, float& fov );
void KrLightPreview_redraw();
bool GameView_on();
bool KrBspPreview_shown();
extern bool g_krGameShaders;
extern bool g_krLightPreview;
extern bool g_skyboxPreview;

namespace
{

struct Sound {
	std::shared_ptr<std::vector<unsigned char>> data;
	ma_decoder decoder{};
	ma_sound sound{};
	bool ready = false;
	Vector3 origin{ 0, 0, 0 };
	bool global = false, loop = false;
	float radius = 0;       // "radius" key: attenuation distance (else the engine default)
	float wait = 0, random = 0, next = 0; // timed speakers
	float volume = 1;
};

ma_engine g_engine;
bool g_engineOk = false;
std::vector<std::unique_ptr<Sound>> g_sounds;
std::map<CopiedString, std::shared_ptr<std::vector<unsigned char>>> g_files;
QTimer* g_timer = nullptr;
float g_clock = 0;
std::mt19937 g_rand( 7 );

bool g_play = false;
ToggleItem g_play_item{ BoolExportCaller( g_play ) };
struct Saved { bool gameView, gameShaders, sky, light, compiled; } g_saved;

/// a sound file from the game's folders / pk3s (the extension may be left out, like in the game)
std::shared_ptr<std::vector<unsigned char>> File_load( const char* name ){
	if( string_empty( name ) || name[0] == '*' )
		return nullptr;
	const auto it = g_files.find( name );
	if( it != g_files.end() )
		return it->second;
	std::shared_ptr<std::vector<unsigned char>> out;
	const char* const ext[] = { "", ".wav", ".mp3" };
	for( const char* e : ext ){
		if( *e != '\0' && !string_empty( path_get_extension( name ) ) )
			break;
		void* buffer = nullptr;
		const std::size_t size = GlobalFileSystem().loadFile( StringStream<128>( name, e ), &buffer );
		if( size != 0 && buffer != nullptr ){
			out = std::make_shared<std::vector<unsigned char>>( static_cast<unsigned char*>( buffer ), static_cast<unsigned char*>( buffer ) + size );
			GlobalFileSystem().freeFile( buffer );
			break;
		}
	}
	g_files[name] = out;
	return out;
}

bool Sound_init( Sound& s ){
	ma_decoder_config config = ma_decoder_config_init( ma_format_f32, 0, 0 );
	if( ma_decoder_init_memory( s.data->data(), s.data->size(), &config, &s.decoder ) != MA_SUCCESS )
		return false;
	if( ma_sound_init_from_data_source( &g_engine, &s.decoder, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &s.sound ) != MA_SUCCESS ){
		ma_decoder_uninit( &s.decoder );
		return false;
	}
	ma_sound_set_looping( &s.sound, s.loop ? MA_TRUE : MA_FALSE );
	s.ready = true;
	return true;
}

/// volume and left / right like the engine: full within 80 units, then down by 0.0008 a unit (or over "radius")
void Sound_place( Sound& s, const Vector3& eye, const Vector3& right ){
	if( !s.ready )
		return;
	float gain = 1, pan = 0;
	if( !s.global ){
		const Vector3 d = s.origin - eye;
		const float dist = std::max( 0.f, float( vector3_length( d ) ) - 80.f );
		gain = s.radius > 0 ? 1 - dist / s.radius : 1 - dist * 0.0008f;
		gain = std::clamp( gain, 0.f, 1.f );
		if( vector3_length( d ) > 1 )
			pan = std::clamp( float( vector3_dot( vector3_normalised( d ), right ) ), -1.f, 1.f ) * 0.8f;
	}
	ma_sound_set_volume( &s.sound, gain * s.volume );
	ma_sound_set_pan( &s.sound, pan );
}

void Sounds_stop(){
	if( g_timer != nullptr )
		g_timer->stop();
	for( auto& s : g_sounds )
		if( s->ready ){
			ma_sound_stop( &s->sound );
			ma_sound_uninit( &s->sound );
			ma_decoder_uninit( &s->decoder );
		}
	g_sounds.clear();
	g_files.clear();
}

void Sounds_tick(){
	g_clock += 0.03f;
	Vector3 eye, angles;
	float fov;
	KrCamera_getView( eye, angles, fov );
	const float yaw = degrees_to_radians( angles[1] );
	const Vector3 right( std::sin( yaw ), -std::cos( yaw ), 0 );
	for( auto& s : g_sounds ){
		Sound_place( *s, eye, right );
		if( s->ready && s->wait > 0 && g_clock >= s->next ){ // timed speaker: again after wait +- random
			ma_sound_seek_to_pcm_frame( &s->sound, 0 );
			ma_sound_start( &s->sound );
			std::uniform_real_distribution<float> spread( -1, 1 );
			s->next = g_clock + std::max( 0.1f, s->wait + spread( g_rand ) * s->random );
		}
	}
}

void Sounds_start(){
	Sounds_stop();
	if( !g_engineOk ){
		g_engineOk = ma_engine_init( nullptr, &g_engine ) == MA_SUCCESS;
		if( !g_engineOk ){
			globalWarningStream() << "play mode: no sound device - the sounds stay quiet\n";
			return;
		}
	}
	int looping = 0, timed = 0, missing = 0, skipped = 0;
	CopiedString music;
	Scene_forEachEntity( [&]( scene::Instance& instance ){
		Entity* e = Node_getEntity( instance.path().top() );
		if( e == nullptr )
			return;
		if( string_equal( e->getClassName(), "worldspawn" ) ){
			music = e->getKeyValue( "music" );
			return;
		}
		if( !string_equal( e->getClassName(), "target_speaker" ) )
			return;
		const char* noise = e->getKeyValue( "noise" );
		if( string_empty( noise ) ){ // soundSet speakers: the game's sound sets are not read here
			++skipped;
			return;
		}
		int flags = 0;
		string_parse_int( e->getKeyValue( "spawnflags" ), flags );
		auto s = std::make_unique<Sound>();
		s->loop = ( flags & 1 ) != 0;
		s->global = ( flags & 4 ) != 0;
		string_parse_float( e->getKeyValue( "wait" ), s->wait );
		string_parse_float( e->getKeyValue( "random" ), s->random );
		string_parse_float( e->getKeyValue( "radius" ), s->radius );
		Vector3 origin;
		if( string_parse_vector3( e->getKeyValue( "origin" ), origin ) )
			s->origin = origin;
		if( !s->loop && s->wait <= 0 ){ // only plays when triggered in game
			++skipped;
			return;
		}
		s->data = File_load( noise );
		if( !s->data ){
			if( missing++ < 5 )
				globalWarningStream() << "play mode: sound not found: " << noise << '\n';
			return;
		}
		if( !Sound_init( *s ) ){
			globalWarningStream() << "play mode: can't decode " << noise << '\n';
			return;
		}
		if( s->loop ){
			ma_sound_start( &s->sound );
			++looping;
		}
		else{
			std::uniform_real_distribution<float> first( 0, std::max( 0.5f, s->wait ) );
			s->next = first( g_rand ); // not all at once
			++timed;
		}
		g_sounds.push_back( std::move( s ) );
	} );
	bool musicOk = false;
	if( !music.empty() ){
		auto s = std::make_unique<Sound>();
		s->loop = true;
		s->global = true;
		s->volume = 0.35f;
		s->data = File_load( music.c_str() );
		if( s->data && Sound_init( *s ) ){
			ma_sound_start( &s->sound );
			g_sounds.push_back( std::move( s ) );
			musicOk = true;
		}
	}
	globalOutputStream() << "play mode: " << looping << " looping and " << timed << " timed speakers"
	                     << ( music.empty() ? "" : musicOk ? ", music " : ", music not found: " ) << music.c_str()
	                     << ( skipped ? StringStream<64>( " (", skipped, " only when triggered / soundSet)" ).c_str() : "" ) << '\n';
	if( g_timer == nullptr ){
		g_timer = new QTimer;
		QObject::connect( g_timer, &QTimer::timeout, [](){ Sounds_tick(); } );
	}
	g_clock = 0;
	Sounds_tick();
	g_timer->start( 30 );
}

void toggleIf( const char* name, bool now, bool want ){
	if( now != want )
		GlobalToggles_find( name ).m_command.m_callback();
}

void Play_toggle(){
	g_play = !g_play;
	g_play_item.update();
	if( g_play ){
		g_saved = { GameView_on(), g_krGameShaders, g_skyboxPreview, g_krLightPreview, KrBspPreview_shown() };
		// the light the game will have: the compile when it is up to date, else the live preview
		const QFileInfo map( Map_Name( g_map ) );
		const QFileInfo bsp( QString( map.path() ) + '/' + map.completeBaseName() + ".bsp" );
		const bool compiled = !Map_Unnamed( g_map ) && bsp.exists() && bsp.lastModified() >= map.lastModified() && !Map_Modified( g_map );
		toggleIf( "ToggleGameView", GameView_on(), true );
		toggleIf( "ToggleGameShaders", g_krGameShaders, true );
		toggleIf( "ToggleSkyboxPreview", g_skyboxPreview, true );
		toggleIf( "ToggleCompiledLighting", KrBspPreview_shown(), compiled );
		toggleIf( "ToggleLightPreview", g_krLightPreview, !compiled );
		globalOutputStream() << "play mode: on - " << ( compiled ? "compiled lighting (.bsp)" : "live light (no up-to-date .bsp - compile for the real thing)" ) << '\n';
		Sounds_start();
	}
	else{
		Sounds_stop();
		toggleIf( "ToggleCompiledLighting", KrBspPreview_shown(), g_saved.compiled );
		toggleIf( "ToggleLightPreview", g_krLightPreview, g_saved.light );
		toggleIf( "ToggleSkyboxPreview", g_skyboxPreview, g_saved.sky );
		toggleIf( "ToggleGameShaders", g_krGameShaders, g_saved.gameShaders );
		toggleIf( "ToggleGameView", GameView_on(), g_saved.gameView );
		globalOutputStream() << "play mode: off\n";
	}
	KrLightPreview_redraw();
}

} // namespace

void KrPlay_registerCommands(){
	GlobalToggles_insert( "TogglePlayMode", makeCallbackF( Play_toggle ), ToggleItem::AddCallbackCaller( g_play_item ) );
}

/// the editor closes: quiet
void KrPlay_shutdown(){
	Sounds_stop();
	if( g_engineOk ){
		ma_engine_uninit( &g_engine );
		g_engineOk = false;
	}
	delete g_timer;
	g_timer = nullptr;
}
