/*
   KenRadiant: Lighting Probe - measures the 3D view's lighting as numbers instead of judging screenshots.

   Record: at the current (levelled) camera, a grid of points over the 3D view gets, from the compiled .bsp,
   the surface (shader, lightmapped or vertex lit), its texture colour, the light q3map2 stored there (lightmap
   texel / vertex colour bytes) and its world position; from the editor the colour it shows and, with the live
   light preview on, the light the preview would store.
   Compare: the game's screenshot from the same spot (setviewpos line) is matched to those points by their world
   positions (eye height, field of view and a small turn are found by themselves) and the game's own formula
   (game colour / ( texture x stored light )) is measured per kind of surface, next to the editor's.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <functional>
#include <QWidget>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QImage>
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QFontDatabase>
#include <QTimer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include "igl.h"
#include "math/matrix.h"
#include "math/vector.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "os/path.h"
#include "commands.h"
#include "gtkutil/dialog.h"
#include "mainframe.h"
#include "map.h"
#include "qerplugin.h"

bool KrBspPreview_drawDebug( int mode, const Matrix4& modelview, const Matrix4& projection );
bool KrBspPreview_batchInfo( int id, CopiedString& shader, bool& lightmapped, bool& translucent, CopiedString& stages );
const char* KrBspPreview_file();
bool KrBspPreview_shown();
void KrCamera_getView( Vector3& origin, Vector3& angles, float& fov );
void KrCamera_setView( const Vector3& origin, const Vector3& angles );
void KrLightPreview_redraw();
QString KrCompile_commands();
void KrCompile_lightParams( float& gamma, float& compensate, float& brightness, float& vertexScale );
bool GameView_on();
const char* EnginePath_get();
extern bool g_krProbeDebug;
extern bool g_krLightPreview;
extern bool g_krGameShaders;
extern int g_krEngineShift;
extern float g_krEngineGamma;

namespace
{

constexpr float c_viewHeightGuess = 38; // setviewpos moves the player; the eye is this much higher (found exactly on Compare)

struct Point {
	int px = 0, py = 0;
	bool hit = false;         // a .bsp surface (not sky / editor-only)
	Vector3 world{ 0, 0, 0 };
	int id = 0;               // batch
	float alpha = 1;          // the surface's texture alpha (decals)
	Vector3 shown{ 0, 0, 0 }; // what the editor showed
	Vector3 albedo{ 0, 0, 0 };
	Vector3 stored{ 0, 0, 0 };
	bool liveOk = false;
	Vector3 live{ 0, 0, 0 };  // the live preview's stored-light estimate
	bool gameOk = false;
	Vector3 game{ 0, 0, 0 };
};
struct Batch { CopiedString shader, stages; bool lightmapped = false, translucent = false; };

struct Record {
	bool valid = false;
	int w = 0, h = 0;
	Vector3 origin{ 0, 0, 0 }, angles{ 0, 0, 0 };
	float fov = 90;
	std::vector<Point> points;
	std::map<int, Batch> batches;
	QString header;
	QString base; // report path without extension
	QImage editorShot;
	bool live = false;
};
Record g_record;

enum class Stage { Idle, Requested, Live };
Stage g_stage = Stage::Idle;
std::function<void( const QString& )> g_onDone;

/// our own single-sample framebuffer: the 3D view's may be multisampled, which can't be read directly
struct ProbeFbo { GLuint fbo = 0, colour = 0, depth = 0; int w = 0, h = 0; GLint previous = 0; };
ProbeFbo g_fbo;
QOpenGLExtraFunctions* fx(){
	return QOpenGLContext::currentContext()->extraFunctions();
}
bool Fbo_ensure( int w, int h ){
	QOpenGLExtraFunctions* f = fx();
	if( g_fbo.fbo != 0 && g_fbo.w == w && g_fbo.h == h )
		return true;
	if( g_fbo.fbo != 0 ){
		f->glDeleteFramebuffers( 1, &g_fbo.fbo );
		f->glDeleteRenderbuffers( 1, &g_fbo.colour );
		f->glDeleteRenderbuffers( 1, &g_fbo.depth );
	}
	g_fbo.w = w; g_fbo.h = h;
	f->glGenRenderbuffers( 1, &g_fbo.colour );
	f->glBindRenderbuffer( GL_RENDERBUFFER, g_fbo.colour );
	f->glRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, w, h );
	f->glGenRenderbuffers( 1, &g_fbo.depth );
	f->glBindRenderbuffer( GL_RENDERBUFFER, g_fbo.depth );
	f->glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h );
	f->glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	f->glGenFramebuffers( 1, &g_fbo.fbo );
	GLint previous = 0;
	gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	f->glBindFramebuffer( GL_FRAMEBUFFER, g_fbo.fbo );
	f->glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_fbo.colour );
	f->glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbo.depth );
	const bool ok = f->glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
	if( !ok )
		globalWarningStream() << "lighting probe: no framebuffer for the readback\n";
	return ok;
}
std::vector<unsigned char> readPixels( int w, int h ){
	std::vector<unsigned char> px( std::size_t( w ) * h * 4 );
	gl().glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	gl().glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	return px;
}
/// what the 3D view shows now (resolved into our framebuffer)
std::vector<unsigned char> readView( int w, int h ){
	QOpenGLExtraFunctions* f = fx();
	GLint previous = 0;
	gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	f->glBindFramebuffer( GL_READ_FRAMEBUFFER, previous );
	f->glBindFramebuffer( GL_DRAW_FRAMEBUFFER, g_fbo.fbo );
	f->glBlitFramebuffer( 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	f->glBindFramebuffer( GL_FRAMEBUFFER, g_fbo.fbo );
	auto px = readPixels( w, h );
	f->glBindFramebuffer( GL_FRAMEBUFFER, previous );
	return px;
}
void Fbo_bind(){
	gl().glGetIntegerv( GL_FRAMEBUFFER_BINDING, &g_fbo.previous );
	fx()->glBindFramebuffer( GL_FRAMEBUFFER, g_fbo.fbo );
}
void Fbo_unbind(){
	fx()->glBindFramebuffer( GL_FRAMEBUFFER, g_fbo.previous );
}
const unsigned char* at( const std::vector<unsigned char>& px, int w, int h, int x, int y ){ // y from the top
	return px.data() + ( std::size_t( h - 1 - y ) * w + x ) * 4;
}
constexpr int c_r = 2; // 5 x 5 pixels per point
Vector3 average( const std::vector<unsigned char>& px, int w, int h, int x, int y, float* alpha = nullptr ){
	Vector3 sum( 0, 0, 0 );
	float a = 0;
	for( int j = -c_r; j <= c_r; ++j )
		for( int i = -c_r; i <= c_r; ++i ){
			const unsigned char* p = at( px, w, h, x + i, y + j );
			sum += Vector3( p[0], p[1], p[2] );
			a += p[3];
		}
	const float n = ( 2 * c_r + 1 ) * ( 2 * c_r + 1 );
	if( alpha != nullptr )
		*alpha = a / n / 255.f;
	return sum / ( n * 255.f );
}

float lum( const Vector3& c ){
	return 0.299f * c.x() + 0.587f * c.y() + 0.114f * c.z();
}
float median( std::vector<float> v, float q = 0.5f ){
	if( v.empty() )
		return 0;
	const std::size_t k = std::min( v.size() - 1, std::size_t( q * ( v.size() - 1 ) + 0.5f ) );
	std::nth_element( v.begin(), v.begin() + k, v.end() );
	return v[k];
}

QString f2( float v ){ return QString::number( v, 'f', 2 ); }
QString f3( float v ){ return QString::number( v, 'f', 3 ); }
QString rgb( const Vector3& c ){ return QString( "%1 %2 %3" ).arg( f3( c.x() ) ).arg( f3( c.y() ) ).arg( f3( c.z() ) ); }

QString gameLine( const Vector3& origin, const Vector3& angles, float fov ){
	return QString( "cg_draw2D 0; cg_drawCrosshair 0; cg_drawGun 0; cg_fov %1; setviewpos %2 %3 %4 %5" )
	       .arg( int( std::lround( fov ) ) )
	       .arg( std::lround( origin.x() ) ).arg( std::lround( origin.y() ) ).arg( std::lround( origin.z() - c_viewHeightGuess ) )
	       .arg( std::lround( angles[1] ) );
}

QString compare( const QString& path );
void finish(){
	Record& r = g_record;
	// batch info and the report
	for( Point& p : r.points )
		if( p.hit && r.batches.find( p.id ) == r.batches.end() ){
			Batch b;
			if( !KrBspPreview_batchInfo( p.id, b.shader, b.lightmapped, b.translucent, b.stages ) )
				p.hit = false;
			else
				r.batches[p.id] = b;
		}
	int hits = 0, live = 0;
	for( const Point& p : r.points ){
		hits += p.hit;
		live += p.hit && p.liveOk;
	}
	QFile file( r.base + ".txt" );
	if( file.open( QIODevice::WriteOnly | QIODevice::Text ) ){
		QTextStream out( &file );
		out << r.header;
		out << "\n# points: px py | world x y z | batch | texture alpha | editor shows rgb | texture rgb | stored light rgb (bsp bytes / 255) | live preview stored-light rgb (or -)\n";
		for( const Point& p : r.points ){
			if( !p.hit )
				continue;
			out << p.px << ' ' << p.py << " | " << f2( p.world.x() ) << ' ' << f2( p.world.y() ) << ' ' << f2( p.world.z() ) << " | " << p.id << " | " << f3( p.alpha )
			    << " | " << rgb( p.shown ) << " | " << rgb( p.albedo ) << " | " << rgb( p.stored ) << " | " << ( p.liveOk ? rgb( p.live ) : QString( "-" ) ) << '\n';
		}
		out << "\n# batches: id | lightmapped | blended | shader | stages\n";
		for( const auto& [id, b] : r.batches )
			out << id << " | " << ( b.lightmapped ? "lightmap" : "vertex" ) << " | " << ( b.translucent ? "blended" : "opaque" ) << " | " << b.shader.c_str() << " | " << b.stages.c_str() << '\n';
	}
	r.editorShot.save( r.base + "_editor.png" );
	r.valid = true;
	QString text = QString( "Recorded %1 points on compiled surfaces (%2 with live preview values).\nReport: %3.txt\n" )
	               .arg( hits ).arg( live ).arg( r.base );
	// the live preview against what q3map2 stored, needs no game
	if( r.live ){
		for( int lm = 1; lm >= 0; --lm ){
			std::vector<float> ratio;
			for( const Point& p : r.points )
				if( p.hit && p.liveOk && r.batches[p.id].lightmapped == bool( lm ) && lum( p.stored ) > 0.02f )
					ratio.push_back( lum( p.live ) / lum( p.stored ) );
			if( ratio.size() >= 10 )
				text += QString( "Live preview vs compiled, %1 surfaces (%2 points): live = %3 x stored (middle half %4 - %5)\n" )
				        .arg( lm ? "lightmapped" : "vertex lit" ).arg( ratio.size() ).arg( f2( median( ratio ) ) ).arg( f2( median( ratio, 0.25f ) ) ).arg( f2( median( ratio, 0.75f ) ) );
		}
	}
	if( const QByteArray test = qgetenv( "KR_PROBE_COMPARE" ); !test.isEmpty() ) // (automatic tests)
		text += compare( QString::fromLocal8Bit( test ) );
	if( g_onDone )
		g_onDone( text );
}

/// the game's screenshot
QImage loadScreenshot( const QString& path ){
	if( path.endsWith( ".tga", Qt::CaseInsensitive ) ){ // own reader: Qt's tga plugin is not always there
		QFile f( path );
		if( !f.open( QIODevice::ReadOnly ) )
			return QImage();
		const QByteArray d = f.readAll();
		if( d.size() < 18 )
			return QImage();
		const auto u8 = [&]( int i ){ return (unsigned char)d[i]; };
		const int idLen = u8( 0 ), type = u8( 2 ), w = u8( 12 ) | u8( 13 ) << 8, h = u8( 14 ) | u8( 15 ) << 8, bpp = u8( 16 ), desc = u8( 17 );
		if( ( type != 2 && type != 10 ) || ( bpp != 24 && bpp != 32 ) || w <= 0 || h <= 0 )
			return QImage();
		const int n = bpp / 8;
		QImage img( w, h, QImage::Format_RGB32 );
		int pos = 18 + idLen;
		std::vector<unsigned char> pix( std::size_t( w ) * h * n );
		if( type == 2 ){
			if( pos + int( pix.size() ) > d.size() )
				return QImage();
			std::memcpy( pix.data(), d.constData() + pos, pix.size() );
		}
		else{
			std::size_t o = 0;
			while( o < pix.size() && pos < d.size() ){
				const int c = u8( pos++ ), count = ( c & 127 ) + 1;
				if( c & 128 ){
					if( pos + n > d.size() ) break;
					for( int k = 0; k < count && o < pix.size(); ++k, o += n )
						std::memcpy( pix.data() + o, d.constData() + pos, n );
					pos += n;
				}
				else{
					for( int k = 0; k < count && o < pix.size() && pos + n <= d.size(); ++k, o += n, pos += n )
						std::memcpy( pix.data() + o, d.constData() + pos, n );
				}
			}
		}
		const bool top = desc & 32;
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x ){
				const unsigned char* p = pix.data() + ( std::size_t( top ? y : h - 1 - y ) * w + x ) * n;
				img.setPixel( x, y, qRgb( p[2], p[1], p[0] ) );
			}
		return img;
	}
	return QImage( path ).convertToFormat( QImage::Format_RGB32 );
}

struct GameView { float s = 1, dz = 0, dyaw = 0, dpitch = 0, dx = 0, dy = 0; };
/// where a world point lands in the game's screenshot
bool project( const Record& r, const GameView& g, int W, int H, const Vector3& p, float& gx, float& gy ){
	const float yaw = degrees_to_radians( r.angles[1] + g.dyaw ), pitch = degrees_to_radians( g.dpitch );
	const Vector3 eye = r.origin + Vector3( g.dx, g.dy, g.dz );
	const float cy = std::cos( yaw ), sy = std::sin( yaw ), cp = std::cos( pitch ), sp = std::sin( pitch );
	const Vector3 f( cp * cy, cp * sy, -sp ), rt( sy, -cy, 0 ), up( sp * cy, sp * sy, cp );
	const Vector3 d = p - eye;
	const float z = vector3_dot( d, f );
	if( z < 1 )
		return false;
	const float t = std::tan( degrees_to_radians( std::round( r.fov ) * 0.5f ) ) * g.s;
	const float xn = vector3_dot( d, rt ) / z / t, yn = vector3_dot( d, up ) / z / ( t * H / W );
	gx = ( xn + 1 ) * 0.5f * W;
	gy = ( 1 - yn ) * 0.5f * H;
	return gx >= 3 && gy >= 3 && gx < W - 4 && gy < H - 4;
}
/// box averages of the screenshot in constant time (summed area tables per channel)
struct Sat {
	int W = 0, H = 0;
	std::vector<double> t[3];
	explicit Sat( const QImage& img ) : W( img.width() ), H( img.height() ){
		for( auto& c : t )
			c.assign( std::size_t( W + 1 ) * ( H + 1 ), 0 );
		for( int y = 0; y < H; ++y ){
			const QRgb* line = reinterpret_cast<const QRgb*>( img.constScanLine( y ) );
			double row[3] = { 0, 0, 0 };
			for( int x = 0; x < W; ++x ){
				row[0] += qRed( line[x] ); row[1] += qGreen( line[x] ); row[2] += qBlue( line[x] );
				for( int k = 0; k < 3; ++k )
					t[k][std::size_t( y + 1 ) * ( W + 1 ) + x + 1] = t[k][std::size_t( y ) * ( W + 1 ) + x + 1] + row[k];
			}
		}
	}
	Vector3 box( float gx, float gy, int r ) const {
		const int x0 = std::clamp( int( gx ) - r, 0, W - 1 ), x1 = std::clamp( int( gx ) + r + 1, 1, W );
		const int y0 = std::clamp( int( gy ) - r, 0, H - 1 ), y1 = std::clamp( int( gy ) + r + 1, 1, H );
		const double n = double( x1 - x0 ) * ( y1 - y0 ) * 255.0;
		Vector3 c;
		for( int k = 0; k < 3; ++k ){
			const auto& a = t[k];
			const auto I = [&]( int x, int y ){ return a[std::size_t( y ) * ( W + 1 ) + x]; };
			c[k] = float( ( I( x1, y1 ) - I( x0, y1 ) - I( x1, y0 ) + I( x0, y0 ) ) / n );
		}
		return c;
	}
};
float Vector3_logLum( const Vector3& c ){
	return std::log( lum( c ) + 0.004f );
}

QString compare( const QString& path ){
	Record& r = g_record;
	if( !r.valid )
		return "Record first (at the same camera you used for the game).";
	const QImage img = loadScreenshot( path );
	if( img.isNull() )
		return "Could not read " + path;
	const int W = img.width(), H = img.height();
	const Sat sat( img );
	const int radius = std::max( 1, int( std::round( 2.f * W / std::max( 1, r.w ) ) ) );
	// alignment: lightmapped opaque points, game brightness against texture x stored light (log), Pearson correlation
	std::vector<const Point*> align;
	for( const Point& p : r.points )
		if( p.hit && r.batches[p.id].lightmapped && !r.batches[p.id].translucent && lum( p.stored ) > 0.01f )
			align.push_back( &p );
	if( align.size() < 30 )
		return "Too few lightmapped surfaces in view - point the camera at walls / floor.";
	if( align.size() > 800 ){ // enough to match, keeps the search quick
		std::vector<const Point*> some;
		for( std::size_t i = 0; i < align.size(); i += align.size() / 800 + 1 )
			some.push_back( align[i] );
		align.swap( some );
	}
	const auto score = [&]( const GameView& g ){
		double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
		int n = 0;
		for( const Point* p : align ){
			float gx, gy;
			if( !project( r, g, W, H, p->world, gx, gy ) )
				continue;
			const float x = Vector3_logLum( Vector3( p->albedo.x() * p->stored.x(), p->albedo.y() * p->stored.y(), p->albedo.z() * p->stored.z() ) );
			const float y = Vector3_logLum( sat.box( gx, gy, radius ) );
			sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; ++n;
		}
		if( n < int( align.size() ) / 3 )
			return -1.0;
		const double cov = sxy / n - sx / n * sy / n, vx = sxx / n - sx / n * sx / n, vy = syy / n - sy / n * sy / n;
		return vx > 0 && vy > 0 ? cov / std::sqrt( vx * vy ) : -1.0;
	};
	GameView best;
	double bestScore = -2;
	for( float s = 0.7f; s <= 1.45f; s += 0.025f )
		for( float dz = -50; dz <= 50; dz += 5 ){
			GameView g;
			g.s = s; g.dz = dz;
			if( const double sc = score( g ); sc > bestScore ){
				bestScore = sc;
				best = g;
			}
		}
	float steps[6] = { 0.0125f, 2.5f, 1, 1, 8, 8 };
	for( int round = 0; round < 8; ++round ){
		bool better = true;
		for( int iter = 0; better && iter < 40; ++iter ){
			better = false;
			for( int k = 0; k < 6; ++k )
				for( float sign : { 1.f, -1.f } ){
					GameView g = best;
					float* v[6] = { &g.s, &g.dz, &g.dyaw, &g.dpitch, &g.dx, &g.dy };
					*v[k] += sign * steps[k];
					if( const double sc = score( g ); sc > bestScore + 1e-6 ){
						bestScore = sc;
						best = g;
						better = true;
					}
				}
		}
		for( float& s : steps )
			s *= 0.5f;
	}
	for( Point& p : r.points ){
		float gx, gy;
		p.gameOk = p.hit && project( r, best, W, H, p.world, gx, gy );
		if( p.gameOk )
			p.game = sat.box( gx, gy, radius );
	}
	QString text = QString( "Matched the screenshot (%1 x %2): field of view x%3, eye %4 units %5 the guess, turned %6 deg, pitch %7 deg, moved %8 %9 - match %10 (1 = perfect)\n" )
	               .arg( W ).arg( H ).arg( f3( best.s ) ).arg( f2( std::fabs( best.dz ) ) ).arg( best.dz >= 0 ? "above" : "below" )
	               .arg( f2( best.dyaw ) ).arg( f2( best.dpitch ) ).arg( f2( best.dx ) ).arg( f2( best.dy ) ).arg( f3( float( bestScore ) ) );
	if( bestScore < 0.8 )
		text += "WARNING: poor match - was the screenshot taken right after the setviewpos line, without moving the mouse?\n";
	// the game's formula per kind of surface
	struct Kind { const char* name; std::function<bool( const Point&, const Batch& )> in; };
	const Kind kinds[] = {
		{ "Lightmapped (walls, floors)", []( const Point&, const Batch& b ){ return b.lightmapped && !b.translucent; } },
		{ "Vertex lit, blended (decals)", []( const Point& p, const Batch& b ){ return !b.lightmapped && b.translucent && p.alpha > 0.9f; } },
		{ "Vertex lit, opaque (models, vertex surfaces)", []( const Point&, const Batch& b ){ return !b.lightmapped && !b.translucent; } },
	};
	QString detail;
	for( const Kind& kind : kinds ){
		std::vector<float> game, editor, ed2game, gr, gg, gb;
		std::vector<std::pair<float, float>> curve; // ( texture x light, game )
		std::map<CopiedString, std::pair<std::vector<float>, std::vector<float>>> perShader;
		for( const Point& p : r.points ){
			if( !p.gameOk || !kind.in( p, r.batches[p.id] ) )
				continue;
			const Vector3 pred( p.albedo.x() * p.stored.x(), p.albedo.y() * p.stored.y(), p.albedo.z() * p.stored.z() );
			const float yp = lum( pred ), yg = lum( p.game ), ye = lum( p.shown );
			if( yp < 0.02f )
				continue;
			curve.emplace_back( yp, yg );
			if( std::max( { p.game.x(), p.game.y(), p.game.z() } ) > 0.97f ) // clipped in game: no ratio
				continue;
			game.push_back( yg / yp );
			editor.push_back( ye / yp );
			if( yg > 0.02f )
				ed2game.push_back( ye / yg );
			if( pred.x() > 0.02f ) gr.push_back( p.game.x() / pred.x() );
			if( pred.y() > 0.02f ) gg.push_back( p.game.y() / pred.y() );
			if( pred.z() > 0.02f ) gb.push_back( p.game.z() / pred.z() );
			auto& ps = perShader[r.batches[p.id].shader];
			ps.first.push_back( yg / yp );
			if( yg > 0.02f )
				ps.second.push_back( ye / yg );
		}
		if( game.size() < 5 ){
			text += QString( "%1: not enough points in view\n" ).arg( kind.name );
			continue;
		}
		text += QString( "%1 (%2 points): GAME = %3 x texture x stored light (middle half %4 - %5; r %6 g %7 b %8). Editor = %9 x -> the editor shows %10 x the game\n" )
		        .arg( kind.name ).arg( game.size() ).arg( f2( median( game ) ) ).arg( f2( median( game, 0.25f ) ) ).arg( f2( median( game, 0.75f ) ) )
		        .arg( f2( median( gr ) ) ).arg( f2( median( gg ) ) ).arg( f2( median( gb ) ) )
		        .arg( f2( median( editor ) ) ).arg( f2( median( ed2game ) ) );
		// the curve: does the game just multiply, or clamp / bend?
		detail += QString( "\n## %1: game brightness by texture x stored light\n#   texture x light | points | game (median) | game / (texture x light)\n" ).arg( kind.name );
		std::sort( curve.begin(), curve.end() );
		for( float lo = 0; lo < 1; lo += 0.05f ){
			std::vector<float> g, k;
			for( const auto& [x, y] : curve )
				if( x >= lo && x < lo + 0.05f ){
					g.push_back( y );
					k.push_back( y / x );
				}
			if( g.size() >= 3 )
				detail += QString( "    %1 - %2 | %3 | %4 | %5\n" ).arg( f2( lo ) ).arg( f2( lo + 0.05f ) ).arg( g.size(), 5 ).arg( f3( median( g ) ) ).arg( f2( median( k ) ) );
		}
		detail += "#   shader | points | game / (texture x light) | editor / game\n";
		std::vector<std::pair<std::size_t, CopiedString>> order;
		for( const auto& [name, v] : perShader )
			order.emplace_back( v.first.size(), name );
		std::sort( order.rbegin(), order.rend() );
		for( std::size_t i = 0; i < order.size() && i < 30; ++i ){
			const auto& v = perShader[order[i].second];
			detail += QString( "    %1 | %2 | %3 | %4\n" ).arg( order[i].second.c_str() ).arg( v.first.size() ).arg( f2( median( v.first ) ) ).arg( f2( median( v.second ) ) );
		}
	}
	QFile file( r.base + "_compare.txt" );
	if( file.open( QIODevice::WriteOnly | QIODevice::Text ) ){
		QTextStream out( &file );
		out << "# KenRadiant Lighting Probe - compared with " << path << '\n' << r.header << '\n' << text << detail;
		out << "\n# points: px py | batch | texture alpha | editor rgb | texture rgb | stored light rgb | live rgb | game rgb\n";
		for( const Point& p : r.points )
			if( p.gameOk )
				out << p.px << ' ' << p.py << " | " << p.id << " | " << f3( p.alpha ) << " | " << rgb( p.shown ) << " | " << rgb( p.albedo ) << " | " << rgb( p.stored )
				    << " | " << ( p.liveOk ? rgb( p.live ) : QString( "-" ) ) << " | " << rgb( p.game ) << '\n';
	}
	return text + QString( "Details: %1_compare.txt\n" ).arg( r.base );
}

QDialog* g_window = nullptr;

/// redraw the 3D view after this frame (a draw request inside a draw is dropped)
void redrawLater(){
	QTimer::singleShot( 0, [](){ KrLightPreview_redraw(); } );
}

} // namespace

/// the 3D view, after its scene: the probe's readbacks (the frame is drawn again afterwards)
void KrProbe_afterScene( const Matrix4& modelview, const Matrix4& projection, int w, int h ){
	if( g_stage == Stage::Idle || w < 32 || h < 32 )
		return;
	Record& r = g_record;
	if( !Fbo_ensure( w, h ) ){
		g_stage = Stage::Idle;
		g_krProbeDebug = false;
		return;
	}
	if( g_stage == Stage::Live ){
		const auto px = readView( w, h );
		for( Point& p : r.points ){
			float a;
			p.live = average( px, w, h, p.px, p.py, &a );
			p.liveOk = a > 0.99f;
		}
		g_krProbeDebug = false;
		g_stage = Stage::Idle;
		finish();
		redrawLater();
		return;
	}
	// normal frame: what the editor shows
	r = Record();
	r.w = w; r.h = h;
	KrCamera_getView( r.origin, r.angles, r.fov );
	const auto shown = readView( w, h );
	r.editorShot = QImage( w, h, QImage::Format_RGB32 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x ){
			const unsigned char* p = at( shown, w, h, x, y );
			r.editorShot.setPixel( x, y, qRgb( p[0], p[1], p[2] ) );
		}
	const int step = std::max( 2 * c_r + 2, w / 96 );
	for( int y = step / 2 + c_r; y < h - c_r - 1; y += step )
		for( int x = step / 2 + c_r; x < w - c_r - 1; x += step ){
			Point p;
			p.px = x; p.py = y;
			p.shown = average( shown, w, h, x, y );
			r.points.push_back( p );
		}
	// the .bsp as data
	const auto clear = [](){
		gl().glClearColor( 0, 0, 0, 0 );
		gl().glDepthMask( GL_TRUE );
		gl().glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	};
	Fbo_bind();
	clear();
	if( !KrBspPreview_drawDebug( 3, modelview, projection ) ){
		Fbo_unbind();
		g_stage = Stage::Idle;
		redrawLater();
		if( g_onDone )
			g_onDone( "No compiled .bsp for this map - compile it first (F9)." );
		return;
	}
	const auto ids = readPixels( w, h );
	std::vector<float> depth( std::size_t( w ) * h );
	gl().glReadPixels( 0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data() );
	const Matrix4 inv = matrix4_full_inverse( matrix4_multiplied_by_matrix4( projection, modelview ) );
	for( Point& p : r.points ){
		const unsigned char* c = at( ids, w, h, p.px, p.py );
		const int id = c[0] | c[1] << 8 | c[2] << 16;
		bool same = id != 0;
		for( int j = -c_r; j <= c_r && same; ++j )
			for( int i = -c_r; i <= c_r && same; ++i ){
				const unsigned char* q = at( ids, w, h, p.px + i, p.py + j );
				same = ( q[0] | q[1] << 8 | q[2] << 16 ) == id;
			}
		const float z = depth[std::size_t( h - 1 - p.py ) * w + p.px];
		if( !same || z >= 1 )
			continue;
		const Vector4 clip( ( p.px + 0.5f ) / w * 2 - 1, 1 - ( p.py + 0.5f ) / h * 2, z * 2 - 1, 1 );
		p.world = vector4_projected( matrix4_transformed_vector4( inv, clip ) );
		p.id = id;
		p.hit = true;
	}
	clear();
	KrBspPreview_drawDebug( 2, modelview, projection );
	const auto albedo = readPixels( w, h );
	clear();
	KrBspPreview_drawDebug( 1, modelview, projection );
	const auto stored = readPixels( w, h );
	Fbo_unbind();
	for( Point& p : r.points )
		if( p.hit ){
			p.albedo = average( albedo, w, h, p.px, p.py, &p.alpha );
			p.stored = average( stored, w, h, p.px, p.py );
		}
	// the report's header: everything that decides the look
	{
		float gamma, compensate, brightness, vscale;
		KrCompile_lightParams( gamma, compensate, brightness, vscale );
		const QFileInfo map( Map_Name( g_map ) ), bsp( KrBspPreview_file() );
		r.base = QFileInfo( map.absolutePath() + "/" + map.completeBaseName() + "_lightprobe" ).absoluteFilePath();
		QString h2;
		QTextStream t( &h2 );
		t << "# KenRadiant Lighting Probe " << QDateTime::currentDateTime().toString( Qt::ISODate ) << " (" << RADIANT_VERSION << ")\n"
		  << "map: " << map.absoluteFilePath() << " (saved " << map.lastModified().toString( Qt::ISODate ) << ")\n"
		  << "bsp: " << bsp.absoluteFilePath() << " (compiled " << bsp.lastModified().toString( Qt::ISODate ) << ")"
		  << ( map.lastModified() > bsp.lastModified() ? "  <- the map was saved after this compile" : "" ) << '\n'
		  << "camera: " << r.origin.x() << ' ' << r.origin.y() << ' ' << r.origin.z() << "  pitch " << r.angles[0] << " yaw " << r.angles[1] << "  fov " << r.fov
		  << "  view " << w << " x " << h << '\n'
		  << "game line: " << gameLine( r.origin, r.angles, r.fov ) << '\n'
		  << "editor: game view " << ( GameView_on() ? "on" : "off" ) << ", game shaders " << ( g_krGameShaders ? "on" : "off" )
		  << ", live light " << ( g_krLightPreview ? "on" : "off" ) << ", compiled lighting " << ( KrBspPreview_shown() ? "on" : "off" )
		  << ", engine overbright shift " << g_krEngineShift << ", r_gamma " << g_krEngineGamma << '\n'
		  << "compile light params: gamma " << gamma << " compensate " << compensate << " brightness " << brightness << " vertexscale " << vscale << '\n'
		  << "compile commands:\n" << KrCompile_commands() << '\n';
		r.header = h2;
	}
	r.live = g_krLightPreview;
	if( r.live ){
		g_krProbeDebug = true; // next frame: the live preview writes its values
		g_stage = Stage::Live;
	}
	else{
		g_stage = Stage::Idle;
		finish();
	}
	redrawLater();
}

void KrProbe_show(){
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint );
		g_window->setWindowTitle( "Lighting Probe" );
		auto *v = new QVBoxLayout( g_window );
		auto *steps = new QLabel(
			"Measures the lighting as numbers: editor and game at the same spot.<br>"
			"<b>1.</b> Compile the map (F9). Set the 3D view up as you want it checked (Game View, Shaders, Light...).<br>"
			"<b>2.</b> Fly to a spot with walls, decals and floor in view, then <b>Level camera</b> and <b>Record</b>.<br>"
			"<b>3.</b> Start SOF2 with the map (<tt>devmap</tt>), paste the game line into the console, press Enter, "
			"don't touch the mouse, <tt>screenshot</tt> (or F12).<br>"
			"<b>4.</b> <b>Compare with game screenshot...</b> and pick it (base/screenshots). Send me the two report files." );
		steps->setWordWrap( true );
		steps->setTextFormat( Qt::RichText );
		v->addWidget( steps );
		auto *row = new QHBoxLayout;
		auto *level = new QPushButton( "Level camera" );
		level->setToolTip( "Pitch 0 and whole units: setviewpos can only make that view in game" );
		auto *line = new QLineEdit;
		line->setReadOnly( true );
		line->setPlaceholderText( "game console line" );
		auto *copy = new QPushButton( "Copy" );
		row->addWidget( level );
		row->addWidget( line, 1 );
		row->addWidget( copy );
		v->addLayout( row );
		auto *row2 = new QHBoxLayout;
		auto *record = new QPushButton( "Record" );
		auto *compareButton = new QPushButton( "Compare with game screenshot..." );
		auto *open = new QPushButton( "Open folder" );
		row2->addWidget( record );
		row2->addWidget( compareButton );
		row2->addStretch( 1 );
		row2->addWidget( open );
		v->addLayout( row2 );
		auto *result = new QPlainTextEdit;
		result->setReadOnly( true );
		result->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		result->setMinimumSize( 760, 220 );
		v->addWidget( result, 1 );
		const auto updateLine = [line](){
			Vector3 o, a;
			float fov;
			KrCamera_getView( o, a, fov );
			line->setText( gameLine( o, a, fov ) );
		};
		QObject::connect( level, &QPushButton::clicked, [updateLine](){
			Vector3 o, a;
			float fov;
			KrCamera_getView( o, a, fov );
			KrCamera_setView( Vector3( std::round( o.x() ), std::round( o.y() ), std::round( o.z() ) ), Vector3( 0, std::round( a[1] ), 0 ) );
			updateLine();
		} );
		QObject::connect( copy, &QPushButton::clicked, [line, updateLine](){
			updateLine();
			QGuiApplication::clipboard()->setText( line->text() );
		} );
		QObject::connect( record, &QPushButton::clicked, [result, updateLine](){
			Vector3 o, a;
			float fov;
			KrCamera_getView( o, a, fov );
			if( std::fabs( a[0] ) > 0.01f )
				result->appendPlainText( "Note: the camera is not level - Compare needs a level camera (setviewpos makes pitch 0)." );
			updateLine();
			result->appendPlainText( "Recording..." );
			g_onDone = [result]( const QString& text ){ result->appendPlainText( text ); };
			g_stage = Stage::Requested;
			KrLightPreview_redraw();
		} );
		QObject::connect( compareButton, &QPushButton::clicked, [result](){
			QString dir = QString( EnginePath_get() ) + "base/screenshots";
			if( !QDir( dir ).exists() )
				dir = EnginePath_get();
			const QString path = QFileDialog::getOpenFileName( g_window, "The game's screenshot", dir, "Screenshots (*.tga *.jpg *.jpeg *.png *.bmp)" );
			if( path.isEmpty() )
				return;
			result->appendPlainText( compare( path ) );
		} );
		QObject::connect( open, &QPushButton::clicked, [](){
			const QFileInfo map( Map_Name( g_map ) );
			QDesktopServices::openUrl( QUrl::fromLocalFile( map.absolutePath() ) );
		} );
		updateLine();
	}
	g_window->show();
	g_window->raise();
	g_window->activateWindow();
}

void KrProbe_registerCommands(){
	GlobalCommands_insert( "KrLightingProbe", makeCallbackF( KrProbe_show ) );
}
