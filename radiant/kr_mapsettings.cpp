/*
   KenRadiant: Map Settings - the worldspawn keys that change how the map looks, with what they do, which compiler
   reads them, and a preview in the 3D view where the editor can show it.

     ambient      flat light added everywhere (q3map2 and sof2map)      -> live light preview
     _color       the colour of ambient / _minlight (both)               -> live light preview
     _minlight    no light goes below this (q3map2)                      -> live light preview
     _floodlight  light from every open direction, q3map2's fill light   -> live light preview (approximate)
     distancecull the game draws nothing further away (q3map2 / sof2map VIS, the game) -> the 3D view cut there
     fog          the map's fog shader                                    -> the Fog window
     gridsize     the light grid models are lit by (both)

   Every change goes into the worldspawn (one undo step each); the previews are switches of the editor only.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QColorDialog>
#include <QTimer>
#include <functional>
#include <vector>
#include <cmath>

#include "ientity.h"
#include "iundo.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "string/string.h"
#include "stringio.h"
#include "stream/stringstream.h"
#include "commands.h"
#include "preferencesystem.h"
#include "math/vector.h"
#include "mainframe.h"

void KrLightPreview_redraw();

/* the editor-only preview switches (saved as preferences) */
bool g_krPreviewAmbient = true;      // ambient / _color / _minlight in the live light preview
bool g_krPreviewFlood = true;        // _floodlight in the live light preview
bool g_krPreviewDistanceCull = false;// cut the 3D view at distancecull

namespace
{

Entity* Worldspawn(){
	Entity* world = nullptr;
	Scene_forEachEntity( [&world]( scene::Instance& instance ){
		Entity* e = Node_getEntity( instance.path().top() );
		if( world == nullptr && e != nullptr && string_equal( e->getClassName(), "worldspawn" ) )
			world = e;
	} );
	return world;
}

/* live edits: written at once for the preview, committed as one undo step when the edit ends */
struct Pending { CopiedString key, original; };
std::vector<Pending> g_pending;

void Key_live( const char* key, const char* value ){
	Entity* w = Worldspawn();
	if( w == nullptr )
		return;
	bool known = false;
	for( const Pending& p : g_pending )
		known = known || string_equal( p.key.c_str(), key );
	if( !known )
		g_pending.push_back( { key, w->getKeyValue( key ) } );
	w->setKeyValue( key, value );
	KrLightPreview_redraw();
}
void Keys_commit(){
	Entity* w = Worldspawn();
	if( w == nullptr || g_pending.empty() ){
		g_pending.clear();
		return;
	}
	std::vector<std::pair<CopiedString, CopiedString>> finals;
	for( const Pending& p : g_pending ){
		finals.emplace_back( p.key, w->getKeyValue( p.key.c_str() ) );
		w->setKeyValue( p.key.c_str(), p.original.c_str() ); // back, outside the undo, so the undo step starts from it
	}
	bool changed = false;
	for( std::size_t i = 0; i < finals.size(); ++i )
		changed = changed || !string_equal( finals[i].second.c_str(), g_pending[i].original.c_str() );
	if( changed ){
		UndoableCommand undo( StringStream<64>( "entitySetKeyValue -key ", g_pending.front().key.c_str() ) );
		for( const auto& [key, value] : finals )
			w->setKeyValue( key.c_str(), value.c_str() );
	}
	g_pending.clear();
	KrLightPreview_redraw();
}
void Key_set( const char* key, const char* value ){
	Key_live( key, value );
	Keys_commit();
}

QString Fmt( double v ){ return QString::number( v, 'g', 6 ); }

/* ---- the window ---- */
QDialog* g_window = nullptr;
std::vector<std::function<void()>> g_readers; // worldspawn -> widgets
QTimer* g_timer = nullptr;
QLabel* g_noWorld = nullptr;

/// a numeric key: slider + spin box + a button that removes the key
void NumberRow( QGridLayout* g, int& r, const char* label, std::vector<const char*> keys, double lo, double hi, double step,
                int decimals, const QString& help ){
	const char* key = keys.front(); // written; the others are read too (q3map2 takes "_ambient" before "ambient")
	auto *name = new QLabel( label );
	name->setToolTip( help );
	g->addWidget( name, r, 0 );
	auto *slider = new QSlider( Qt::Horizontal );
	slider->setRange( int( lo / step ), int( hi / step ) );
	slider->setToolTip( help );
	g->addWidget( slider, r, 1 );
	auto *spin = new QDoubleSpinBox;
	spin->setRange( lo, hi );
	spin->setDecimals( decimals );
	spin->setSingleStep( step );
	spin->setSpecialValueText( "off" );
	spin->setToolTip( help );
	g->addWidget( spin, r, 2 );
	auto *del = new QPushButton( "Remove" );
	del->setToolTip( QString( "Remove \"%1\" from the worldspawn" ).arg( key ) );
	g->addWidget( del, r, 3 );
	auto *desc = new QLabel( help );
	desc->setWordWrap( true );
	desc->setStyleSheet( "color: gray;" );
	g->addWidget( desc, r + 1, 1, 1, 3 );
	r += 2;
	// which key the map uses (an existing "_ambient" is edited, not shadowed)
	auto used = std::make_shared<CopiedString>( key );
	const auto write = [used, spin]( double v, bool commit ){
		const QString text = v <= spin->minimum() ? QString() : Fmt( v );
		Key_live( used->c_str(), text.toLatin1().constData() );
		if( commit )
			Keys_commit();
	};
	QObject::connect( slider, &QSlider::valueChanged, [spin, step, write]( int i ){
		const QSignalBlocker b( spin );
		spin->setValue( i * step );
		write( i * step, false );
	} );
	QObject::connect( slider, &QSlider::sliderReleased, [](){ Keys_commit(); } );
	QObject::connect( spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [slider, step, write]( double v ){
		const QSignalBlocker b( slider );
		slider->setValue( int( std::lround( v / step ) ) );
		write( v, false );
	} );
	QObject::connect( spin, &QDoubleSpinBox::editingFinished, [](){ Keys_commit(); } );
	QObject::connect( del, &QPushButton::clicked, [used](){ Key_set( used->c_str(), "" ); } );
	g_readers.push_back( [keys, used, slider, spin, del](){
		Entity* w = Worldspawn();
		*used = keys.front();
		double v = spin->minimum();
		bool has = false;
		if( w != nullptr )
			for( const char* k : keys ){
				float f;
				if( !string_empty( w->getKeyValue( k ) ) && string_parse_float( w->getKeyValue( k ), f ) ){
					v = f; has = true; *used = k;
					break;
				}
			}
		if( spin->hasFocus() || slider->isSliderDown() )
			return;
		const QSignalBlocker b1( spin ), b2( slider );
		spin->setValue( v );
		slider->setValue( int( std::lround( v / spin->singleStep() ) ) );
		del->setEnabled( has );
	} );
}

QPushButton* ColourButton(){
	auto *b = new QPushButton;
	b->setMinimumWidth( 80 );
	return b;
}
void ColourButton_show( QPushButton* b, const Vector3& c, bool on ){
	const QColor qc( int( std::clamp( c.x(), 0.f, 1.f ) * 255 + 0.5f ), int( std::clamp( c.y(), 0.f, 1.f ) * 255 + 0.5f ),
	                 int( std::clamp( c.z(), 0.f, 1.f ) * 255 + 0.5f ) );
	b->setText( on ? QString( "%1 %2 %3" ).arg( Fmt( c.x() ), Fmt( c.y() ), Fmt( c.z() ) ) : "white (not set)" );
	b->setStyleSheet( on ? QString( "background: %1; color: %2;" ).arg( qc.name() ).arg( qc.lightness() > 128 ? "#202020" : "#e0e0e0" ) : "" );
}
/// a colour picker that follows the drag live and commits on OK
void Colour_pick( const Vector3& start, const std::function<void(const Vector3&)>& live, const std::function<void(bool)>& done ){
	QColorDialog dialog( g_window );
	dialog.setOption( QColorDialog::DontUseNativeDialog );
	dialog.setCurrentColor( QColor::fromRgbF( start.x(), start.y(), start.z() ) );
	QObject::connect( &dialog, &QColorDialog::currentColorChanged, [live]( const QColor& c ){
		if( c.isValid() )
			live( Vector3( float( c.redF() ), float( c.greenF() ), float( c.blueF() ) ) );
	} );
	done( dialog.exec() == QDialog::Accepted );
}

void Window_read(){
	if( g_window == nullptr )
		return;
	if( !g_window->isVisible() ){
		g_timer->stop();
		return;
	}
	g_noWorld->setVisible( Worldspawn() == nullptr );
	for( auto& r : g_readers )
		r();
}

QCheckBox* PreviewBox( const char* text, bool& value, const QString& help ){
	auto *b = new QCheckBox( text );
	b->setToolTip( help );
	b->setChecked( value );
	QObject::connect( b, &QCheckBox::toggled, [&value]( bool on ){ value = on; KrLightPreview_redraw(); } );
	return b;
}

void MapSettings_show(){
	if( g_window == nullptr ){
		g_window = new QDialog( MainFrame_getWindow(), Qt::Window );
		g_window->setWindowTitle( "Map Settings (worldspawn)" );
		auto *v = new QVBoxLayout( g_window );
		g_noWorld = new QLabel( "No map open." );
		v->addWidget( g_noWorld );

		/* ---- light ---- */
		auto *light = new QGroupBox( "Light - used when you compile (LIGHT stage)" );
		auto *lg = new QGridLayout( light );
		int r = 0;
		NumberRow( lg, r, "ambient", { "ambient", "_ambient" }, 0, 255, 1, 0,
		           "Flat light added to every surface, 0-255 (q3map2 and sof2map). Lifts the darkest shadows evenly." );
		{
			lg->addWidget( new QLabel( "_color" ), r, 0 );
			auto *b = ColourButton();
			b->setToolTip( "The colour of ambient and _minlight (q3map2 and sof2map). White when not set." );
			lg->addWidget( b, r, 1, 1, 2 );
			auto *del = new QPushButton( "Remove" );
			lg->addWidget( del, r, 3 );
			QObject::connect( del, &QPushButton::clicked, [](){ Key_set( "_color", "" ); } );
			const auto current = [](){
				Vector3 c( 1, 1, 1 );
				if( Entity* w = Worldspawn() )
					string_parse_vector3( w->getKeyValue( "_color" ), c );
				return c;
			};
			QObject::connect( b, &QPushButton::clicked, [current](){
				const Vector3 start = current();
				Colour_pick( start, []( const Vector3& c ){
					Key_live( "_color", QString( "%1 %2 %3" ).arg( Fmt( c.x() ), Fmt( c.y() ), Fmt( c.z() ) ).toLatin1().constData() );
				}, []( bool ok ){
					if( ok )
						Keys_commit();
					else{ // cancelled: back to how it was
						for( const Pending& p : g_pending )
							if( Entity* w = Worldspawn() )
								w->setKeyValue( p.key.c_str(), p.original.c_str() );
						g_pending.clear();
						KrLightPreview_redraw();
					}
				} );
			} );
			g_readers.push_back( [b, del](){
				Entity* w = Worldspawn();
				Vector3 c( 1, 1, 1 );
				const bool on = w != nullptr && string_parse_vector3( w->getKeyValue( "_color" ), c );
				ColourButton_show( b, c, on );
				del->setEnabled( on );
			} );
			++r;
		}
		NumberRow( lg, r, "_minlight", { "_minlight" }, 0, 255, 1, 0,
		           "No surface ends up darker than this, 0-255 (q3map2 only - sof2map ignores it)." );
		{
			auto *fl = new QCheckBox( "_floodlight" );
			fl->setToolTip( "q3map2's fill light: light from every direction that is open within the distance - like a soft "
			                "overcast sky that also reaches under roofs a little. q3map2 only, sof2map ignores it." );
			lg->addWidget( fl, r, 0 );
			auto *col = ColourButton();
			lg->addWidget( col, r, 1, 1, 2 );
			++r;
			auto *dl = new QLabel( "  distance" );
			lg->addWidget( dl, r, 0 );
			auto *dist = new QSlider( Qt::Horizontal );
			dist->setRange( 64, 8192 );
			dist->setToolTip( "How far a direction has to be open to count (q3map2 default 1024)." );
			lg->addWidget( dist, r, 1 );
			auto *distSpin = new QDoubleSpinBox;
			distSpin->setRange( 64, 8192 );
			distSpin->setDecimals( 0 );
			distSpin->setSingleStep( 64 );
			lg->addWidget( distSpin, r, 2 );
			++r;
			auto *il = new QLabel( "  intensity" );
			lg->addWidget( il, r, 0 );
			auto *inten = new QSlider( Qt::Horizontal );
			inten->setRange( 1, 1024 );
			inten->setToolTip( "How much light a fully open spot gets (q3map2 default 128)." );
			lg->addWidget( inten, r, 1 );
			auto *intenSpin = new QDoubleSpinBox;
			intenSpin->setRange( 1, 1024 );
			intenSpin->setDecimals( 0 );
			lg->addWidget( intenSpin, r, 2 );
			++r;
			struct Flood { bool on = false; Vector3 colour{ 0.94f, 0.94f, 1 }; float distance = 1024, intensity = 128; };
			const auto read = [](){
				Flood f;
				if( Entity* w = Worldspawn() ){
					const char* v = w->getKeyValue( "_floodlight" );
					if( !string_empty( v ) ){
						f.on = true;
						double a = 0, b = 0, c = 0, d = 1024, e = 128;
						const int n = sscanf( v, "%lf %lf %lf %lf %lf", &a, &b, &c, &d, &e );
						if( n >= 3 && ( a > 0 || b > 0 || c > 0 ) ) f.colour = Vector3( float( a ), float( b ), float( c ) );
						if( n >= 4 && d >= 1 ) f.distance = float( d );
						if( n >= 5 && e >= 1 ) f.intensity = float( e );
					}
				}
				return f;
			};
			const auto text = []( const Flood& f ){
				return QString( "%1 %2 %3 %4 %5" ).arg( Fmt( f.colour.x() ), Fmt( f.colour.y() ), Fmt( f.colour.z() ) )
				       .arg( Fmt( f.distance ), Fmt( f.intensity ) ).toLatin1();
			};
			QObject::connect( fl, &QCheckBox::toggled, [read, text]( bool on ){
				Flood f = read();
				Key_set( "_floodlight", on ? text( f ).constData() : "" );
			} );
			QObject::connect( col, &QPushButton::clicked, [read, text](){
				const Flood start = read();
				Colour_pick( start.colour, [read, text]( const Vector3& c ){
					Flood f = read(); f.on = true; f.colour = c;
					Key_live( "_floodlight", text( f ).constData() );
				}, []( bool ok ){
					if( ok ) Keys_commit();
					else{
						for( const Pending& p : g_pending )
							if( Entity* w = Worldspawn() ) w->setKeyValue( p.key.c_str(), p.original.c_str() );
						g_pending.clear();
						KrLightPreview_redraw();
					}
				} );
			} );
			const auto liveNumber = [read, text]( bool distance, double v ){
				Flood f = read(); f.on = true;
				( distance ? f.distance : f.intensity ) = float( v );
				Key_live( "_floodlight", text( f ).constData() );
			};
			QObject::connect( dist, &QSlider::valueChanged, [distSpin, liveNumber]( int i ){ const QSignalBlocker b( distSpin ); distSpin->setValue( i ); liveNumber( true, i ); } );
			QObject::connect( distSpin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [dist, liveNumber]( double d ){ const QSignalBlocker b( dist ); dist->setValue( int( d ) ); liveNumber( true, d ); } );
			QObject::connect( inten, &QSlider::valueChanged, [intenSpin, liveNumber]( int i ){ const QSignalBlocker b( intenSpin ); intenSpin->setValue( i ); liveNumber( false, i ); } );
			QObject::connect( intenSpin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [inten, liveNumber]( double d ){ const QSignalBlocker b( inten ); inten->setValue( int( d ) ); liveNumber( false, d ); } );
			for( QSlider* s : { dist, inten } )
				QObject::connect( s, &QSlider::sliderReleased, [](){ Keys_commit(); } );
			for( QDoubleSpinBox* s : { distSpin, intenSpin } )
				QObject::connect( s, &QDoubleSpinBox::editingFinished, [](){ Keys_commit(); } );
			g_readers.push_back( [read, fl, col, dist, distSpin, inten, intenSpin](){
				const Flood f = read();
				const QSignalBlocker b1( fl ), b2( dist ), b3( distSpin ), b4( inten ), b5( intenSpin );
				fl->setChecked( f.on );
				ColourButton_show( col, f.colour, f.on );
				if( !dist->isSliderDown() && !distSpin->hasFocus() ){ dist->setValue( int( f.distance ) ); distSpin->setValue( f.distance ); }
				if( !inten->isSliderDown() && !intenSpin->hasFocus() ){ inten->setValue( int( f.intensity ) ); intenSpin->setValue( f.intensity ); }
				for( QWidget* w : std::initializer_list<QWidget*>{ col, dist, distSpin, inten, intenSpin } )
					w->setEnabled( f.on );
			} );
		}
		auto *previews = new QHBoxLayout;
		previews->addWidget( PreviewBox( "Show ambient / _color / _minlight in the light preview", g_krPreviewAmbient,
		                                 "Off: the live light preview (Ctrl+L) leaves them out, so you see the lights and the sun alone." ) );
		previews->addWidget( PreviewBox( "Show _floodlight (approximate)", g_krPreviewFlood,
		                                 "The preview adds the flood light by how much sky a spot sees - close to q3map2 outdoors; "
		                                 "q3map2 also counts walls further away than the distance as open, so rooms come out a little "
		                                 "brighter after compiling." ) );
		lg->addLayout( previews, r++, 0, 1, 4 );
		auto *lnote = new QLabel( "This light only shows in the live light preview (Ctrl+L) - compile to see the real thing." );
		lnote->setStyleSheet( "color: gray;" );
		lg->addWidget( lnote, r++, 0, 1, 4 );
		lg->setColumnStretch( 1, 1 );
		v->addWidget( light );

		/* ---- view ---- */
		auto *view = new QGroupBox( "In game" );
		auto *vg = new QGridLayout( view );
		r = 0;
		NumberRow( vg, r, "distancecull", { "distancecull" }, 0, 32768, 64, 0,
		           "The game draws nothing further away than this (and VIS throws away what is beyond - q3map2 and sof2map). "
		           "With fog, put it where the fog is nearly full, so the cut can't be seen." );
		vg->addWidget( PreviewBox( "Cut the 3D view at distancecull, like the game", g_krPreviewDistanceCull,
		                           "The editor's camera stops drawing at that distance. With the map's fog on (Fog window) the "
		                           "edge disappears into the fog the way it does in game." ), r++, 1, 1, 3 );
		{
			vg->addWidget( new QLabel( "fog" ), r, 0 );
			auto *name = new QLabel;
			vg->addWidget( name, r, 1, 1, 2 );
			auto *b = new QPushButton( "Fog..." );
			b->setToolTip( "The Fog window: show the map's fog in the 3D view, try other fogs." );
			QObject::connect( b, &QPushButton::clicked, [](){ GlobalCommands_find( "KrFogWindow" ).m_callback(); } );
			vg->addWidget( b, r, 3 );
			g_readers.push_back( [name](){
				Entity* w = Worldspawn();
				const char* f = w != nullptr ? w->getKeyValue( "fog" ) : "";
				name->setText( string_empty( f ) ? QString( "none" ) : QString( f ) );
			} );
			++r;
		}
		{
			vg->addWidget( new QLabel( "gridsize" ), r, 0 );
			auto *e = new QLineEdit;
			e->setPlaceholderText( "64 64 128 (the default)" );
			e->setToolTip( "The spacing of the light grid models and players are lit by: \"X Y Z\". Bigger = fewer grid points "
			               "(smaller .bsp, blurrier model light). q3map2 and sof2map." );
			vg->addWidget( e, r, 1, 1, 2 );
			QObject::connect( e, &QLineEdit::editingFinished, [e](){
				Key_set( "gridsize", e->text().trimmed().toLatin1().constData() );
			} );
			g_readers.push_back( [e](){
				if( e->hasFocus() )
					return;
				Entity* w = Worldspawn();
				e->setText( w != nullptr ? w->getKeyValue( "gridsize" ) : "" );
			} );
			++r;
		}
		vg->setColumnStretch( 1, 1 );
		v->addWidget( view );

		g_window->setMinimumWidth( 640 );
		g_timer = new QTimer( g_window ); // undo / redo / the entity inspector change the keys too
		g_timer->setInterval( 500 );
		QObject::connect( g_timer, &QTimer::timeout, Window_read );
	}
	g_window->show();
	g_window->raise();
	g_timer->start();
	Window_read();
}

} // namespace

/* ---- for the 3D view ---- */

/// distancecull when the view should be cut there, else 0
float KrMapSettings_distanceCull(){
	if( !g_krPreviewDistanceCull )
		return 0;
	float d = 0;
	if( Entity* w = Worldspawn() )
		string_parse_float( w->getKeyValue( "distancecull" ), d );
	return std::max( d, 0.f );
}

void KrMapSettings_registerCommands(){
	GlobalCommands_insert( "KrMapSettings", makeCallbackF( MapSettings_show ) );
	GlobalPreferenceSystem().registerPreference( "KrPreviewAmbient", BoolImportStringCaller( g_krPreviewAmbient ), BoolExportStringCaller( g_krPreviewAmbient ) );
	GlobalPreferenceSystem().registerPreference( "KrPreviewFlood", BoolImportStringCaller( g_krPreviewFlood ), BoolExportStringCaller( g_krPreviewFlood ) );
	GlobalPreferenceSystem().registerPreference( "KrPreviewDistanceCull", BoolImportStringCaller( g_krPreviewDistanceCull ), BoolExportStringCaller( g_krPreviewDistanceCull ) );
}
