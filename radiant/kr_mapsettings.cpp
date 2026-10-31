/*
   KenRadiant: Worldspawn settings - the worldspawn keys that change how the map looks, with what they do, which
   compiler reads them, and a preview in the 3D view where the editor can show it; plus every other worldspawn key
   in a table.

     ambient      flat light added everywhere (q3map2 and sof2map)      -> live light preview
     _color       the colour of ambient / _minlight (both)               -> live light preview
     _minlight    no light goes below this (q3map2)                      -> live light preview
     _floodlight  light from every open direction, q3map2's fill light   -> live light preview (approximate)
     distancecull the game draws nothing further away (q3map2 / sof2map VIS, the game) -> the 3D view cut there
     fog          the map's fog shader                                    -> the Fog window
     gridsize     the light grid models are lit by (both)

   The controls are built as sections that can sit in more than one place: the "Worldspawn" tab (next to
   Textures / Models / Entities, or a page of the group window in the other layouts) has all of them, the Sun
   Calibrator has the light section. Changes are written at once (live) and become one undo step when the edit
   ends. The keys as they were when the map was loaded are remembered, so any key - or all of them - can be put
   back ("Reset").

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QWidget>
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
#include <QScrollArea>
#include <QTableWidget>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <functional>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>

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
#include "groupdialog.h"

void KrLightPreview_redraw();
extern QTabWidget* g_texModelTabs;

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

using KeyValues = std::vector<std::pair<CopiedString, CopiedString>>;

KeyValues Keys_of( const Entity* e ){
	struct V : Entity::Visitor {
		KeyValues kv;
		void visit( const char* key, const char* value ) override { kv.emplace_back( key, value ); }
	} v;
	if( e != nullptr )
		e->forEachKeyValue( v );
	return v.kv;
}

/* ---- the keys as loaded (what Reset goes back to) ---- */
struct Snapshot {
	bool valid = false;
	const Entity* world = nullptr; // a different worldspawn = another map: a new snapshot
	KeyValues keys;
	const char* get( const char* key, bool& has ) const {
		for( const auto& [k, v] : keys )
			if( string_equal_nocase( k.c_str(), key ) ){
				has = true;
				return v.c_str();
			}
		has = false;
		return "";
	}
	const char* get( const char* key ) const { bool has; return get( key, has ); }
} g_snapshot;

void Snapshot_take(){
	const Entity* w = Worldspawn();
	g_snapshot.valid = true;
	g_snapshot.world = w;
	g_snapshot.keys = Keys_of( w );
}
void Snapshot_check(){
	if( !g_snapshot.valid || g_snapshot.world != Worldspawn() )
		Snapshot_take();
}
bool Key_changed( const char* key ){
	const Entity* w = Worldspawn();
	return w != nullptr && !string_equal( w->getKeyValue( key ), g_snapshot.get( key ) );
}

/* ---- live edits: written at once for the preview, committed as one undo step when the edit ends ---- */
struct Pending { CopiedString key, original; };
std::vector<Pending> g_pending;

void Key_live( const char* key, const char* value ){
	Entity* w = Worldspawn();
	if( w == nullptr || string_equal_nocase( key, "classname" ) )
		return;
	bool known = false;
	for( const Pending& p : g_pending )
		known = known || string_equal( p.key.c_str(), key );
	if( !known )
		g_pending.push_back( { key, w->getKeyValue( key ) } );
	w->setKeyValue( key, value );
	KrLightPreview_redraw();
}
void Keys_commit( const char* undoName = nullptr ){
	Entity* w = Worldspawn();
	if( w == nullptr || g_pending.empty() ){
		g_pending.clear();
		return;
	}
	KeyValues finals;
	for( const Pending& p : g_pending ){
		finals.emplace_back( p.key, w->getKeyValue( p.key.c_str() ) );
		w->setKeyValue( p.key.c_str(), p.original.c_str() ); // back, outside the undo, so the undo step starts from it
	}
	bool changed = false;
	for( std::size_t i = 0; i < finals.size(); ++i )
		changed = changed || !string_equal( finals[i].second.c_str(), g_pending[i].original.c_str() );
	if( changed ){
		const auto name = undoName != nullptr ? StringStream<64>( undoName ) : StringStream<64>( "entitySetKeyValue -key ", g_pending.front().key.c_str() );
		UndoableCommand undo( name ); // the name has to outlive the command: it is read when the command ends
		for( const auto& [key, value] : finals )
			w->setKeyValue( key.c_str(), value.c_str() );
	}
	g_pending.clear();
	KrLightPreview_redraw();
}
/// a live edit that was cancelled: back to how it was, nothing in the undo
void Keys_revert(){
	if( Entity* w = Worldspawn() )
		for( const Pending& p : g_pending )
			w->setKeyValue( p.key.c_str(), p.original.c_str() );
	g_pending.clear();
	KrLightPreview_redraw();
}
void Key_set( const char* key, const char* value ){
	Key_live( key, value );
	Keys_commit();
}
/// these keys back to how they were when the map was loaded, one undo step
void Keys_reset( const std::vector<CopiedString>& keys ){
	Keys_commit();
	for( const CopiedString& k : keys )
		Key_live( k.c_str(), g_snapshot.get( k.c_str() ) );
	Keys_commit( "worldspawnReset" );
}
/// every key back as loaded (keys added since are removed)
void Keys_resetAll(){
	std::vector<CopiedString> keys;
	for( const auto& [k, v] : Keys_of( Worldspawn() ) )
		keys.push_back( k );
	for( const auto& [k, v] : g_snapshot.keys )
		keys.push_back( k );
	Keys_reset( keys );
}

QString Fmt( double v ){ return QString::number( v, 'g', 6 ); }

/* ---- panels: a group of controls; whatever is visible is refreshed from the worldspawn ---- */
struct Panel {
	QWidget* root = nullptr;
	std::vector<std::function<void()>> readers; // worldspawn -> widgets
};
std::vector<std::unique_ptr<Panel>> g_panels;
QTimer* g_timer = nullptr;

void Panels_read(){
	Snapshot_check();
	for( auto& p : g_panels )
		if( p->root->isVisible() )
			for( auto& r : p->readers )
				r();
}
Panel& Panel_new( QWidget* root ){
	g_panels.push_back( std::make_unique<Panel>() );
	Panel* p = g_panels.back().get();
	p->root = root;
	QObject::connect( root, &QObject::destroyed, [p](){
		g_panels.erase( std::remove_if( g_panels.begin(), g_panels.end(), [p]( auto& q ){ return q.get() == p; } ), g_panels.end() );
	} );
	if( g_timer == nullptr ){ // undo / redo / the entity inspector change the keys too
		g_timer = new QTimer( MainFrame_getWindow() );
		g_timer->setInterval( 400 );
		QObject::connect( g_timer, &QTimer::timeout, Panels_read );
		g_timer->start();
	}
	return *p;
}

/// "Reset": back to the value the map was loaded with; enabled while it differs
QPushButton* ResetButton( Panel& panel, std::vector<CopiedString> keys ){
	auto *b = new QPushButton( "Reset" );
	b->setToolTip( QString( "Back to how \"%1\" was when the map was loaded" ).arg( keys.front().c_str() ) );
	QObject::connect( b, &QPushButton::clicked, [keys](){ Keys_reset( keys ); } );
	panel.readers.push_back( [b, keys](){
		bool changed = false;
		for( const CopiedString& k : keys )
			changed = changed || Key_changed( k.c_str() );
		b->setEnabled( changed );
		if( changed ){
			bool has;
			const char* was = g_snapshot.get( keys.front().c_str(), has );
			b->setToolTip( QString( "Back to how it was when the map was loaded: %1" ).arg( has ? QString( "\"%1\"" ).arg( was ) : QString( "not set" ) ) );
		}
	} );
	return b;
}

/// a numeric key: slider + spin box + Remove + Reset
void NumberRow( Panel& panel, QGridLayout* g, int& r, const char* label, std::vector<const char*> keys, double lo, double hi, double step,
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
	std::vector<CopiedString> all;
	for( const char* k : keys )
		all.emplace_back( k );
	g->addWidget( ResetButton( panel, all ), r, 4 );
	auto *desc = new QLabel( help );
	desc->setWordWrap( true );
	desc->setStyleSheet( "color: gray;" );
	g->addWidget( desc, r + 1, 1, 1, 4 );
	r += 2;
	// which key the map uses (an existing "_ambient" is edited, not shadowed)
	auto used = std::make_shared<CopiedString>( key );
	const auto write = [used, spin]( double v ){
		const QString text = v <= spin->minimum() ? QString() : Fmt( v );
		Key_live( used->c_str(), text.toLatin1().constData() );
	};
	QObject::connect( slider, &QSlider::valueChanged, [spin, step, write]( int i ){
		const QSignalBlocker b( spin );
		spin->setValue( i * step );
		write( i * step );
	} );
	QObject::connect( slider, &QSlider::sliderReleased, [](){ Keys_commit(); } );
	QObject::connect( spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [slider, step, write]( double v ){
		const QSignalBlocker b( slider );
		slider->setValue( int( std::lround( v / step ) ) );
		write( v );
	} );
	QObject::connect( spin, &QDoubleSpinBox::editingFinished, [](){ Keys_commit(); } );
	QObject::connect( del, &QPushButton::clicked, [used](){ Key_set( used->c_str(), "" ); } );
	panel.readers.push_back( [keys, used, slider, spin, del](){
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
		del->setEnabled( has );
		if( spin->hasFocus() || slider->isSliderDown() )
			return;
		const QSignalBlocker b1( spin ), b2( slider );
		spin->setValue( v );
		slider->setValue( int( std::lround( v / spin->singleStep() ) ) );
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
/// a colour picker that follows the drag live; OK commits, Cancel puts it back
void Colour_pick( QWidget* parent, const Vector3& start, const std::function<void(const Vector3&)>& live ){
	QColorDialog dialog( parent );
	dialog.setOption( QColorDialog::DontUseNativeDialog );
	dialog.setCurrentColor( QColor::fromRgbF( start.x(), start.y(), start.z() ) );
	QObject::connect( &dialog, &QColorDialog::currentColorChanged, [live]( const QColor& c ){
		if( c.isValid() )
			live( Vector3( float( c.redF() ), float( c.greenF() ), float( c.blueF() ) ) );
	} );
	if( dialog.exec() == QDialog::Accepted )
		Keys_commit();
	else
		Keys_revert();
}

QCheckBox* PreviewBox( Panel& panel, const char* text, bool& value, const QString& help ){
	auto *b = new QCheckBox( text );
	b->setToolTip( help );
	b->setChecked( value );
	QObject::connect( b, &QCheckBox::toggled, [&value]( bool on ){ value = on; KrLightPreview_redraw(); } );
	panel.readers.push_back( [b, &value](){ // the same switch can be in more than one panel
		const QSignalBlocker block( b );
		b->setChecked( value );
	} );
	return b;
}

/* ---- the sections ---- */

QGroupBox* LightSection( Panel& panel, const char* title ){
	auto *light = new QGroupBox( title );
	auto *lg = new QGridLayout( light );
	int r = 0;
	NumberRow( panel, lg, r, "ambient", { "ambient", "_ambient" }, 0, 255, 1, 0,
	           "Flat light added to every surface, 0-255 (q3map2 and sof2map). Lifts the darkest shadows evenly." );
	{
		lg->addWidget( new QLabel( "_color" ), r, 0 );
		auto *b = ColourButton();
		b->setToolTip( "The colour of ambient and _minlight (q3map2 and sof2map). White when not set." );
		lg->addWidget( b, r, 1, 1, 2 );
		auto *del = new QPushButton( "Remove" );
		lg->addWidget( del, r, 3 );
		lg->addWidget( ResetButton( panel, { "_color" } ), r, 4 );
		QObject::connect( del, &QPushButton::clicked, [](){ Key_set( "_color", "" ); } );
		QObject::connect( b, &QPushButton::clicked, [b](){
			Vector3 start( 1, 1, 1 );
			if( Entity* w = Worldspawn() )
				string_parse_vector3( w->getKeyValue( "_color" ), start );
			Colour_pick( b->window(), start, []( const Vector3& c ){
				Key_live( "_color", QString( "%1 %2 %3" ).arg( Fmt( c.x() ), Fmt( c.y() ), Fmt( c.z() ) ).toLatin1().constData() );
			} );
		} );
		panel.readers.push_back( [b, del](){
			Entity* w = Worldspawn();
			Vector3 c( 1, 1, 1 );
			const bool on = w != nullptr && string_parse_vector3( w->getKeyValue( "_color" ), c );
			ColourButton_show( b, c, on );
			del->setEnabled( on );
		} );
		++r;
	}
	NumberRow( panel, lg, r, "_minlight", { "_minlight" }, 0, 255, 1, 0,
	           "No surface ends up darker than this, 0-255 (q3map2 only - sof2map ignores it)." );
	{
		auto *fl = new QCheckBox( "_floodlight" );
		fl->setToolTip( "q3map2's fill light: light from every direction that is open within the distance - like a soft "
		                "overcast sky that also reaches under roofs a little. q3map2 only, sof2map ignores it." );
		lg->addWidget( fl, r, 0 );
		auto *col = ColourButton();
		lg->addWidget( col, r, 1, 1, 2 );
		lg->addWidget( ResetButton( panel, { "_floodlight" } ), r, 4 );
		++r;
		lg->addWidget( new QLabel( "  distance" ), r, 0 );
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
		lg->addWidget( new QLabel( "  intensity" ), r, 0 );
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
		QObject::connect( col, &QPushButton::clicked, [col, read, text](){
			Colour_pick( col->window(), read().colour, [read, text]( const Vector3& c ){
				Flood f = read(); f.on = true; f.colour = c;
				Key_live( "_floodlight", text( f ).constData() );
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
		panel.readers.push_back( [read, fl, col, dist, distSpin, inten, intenSpin](){
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
	auto *previews = new QVBoxLayout;
	previews->addWidget( PreviewBox( panel, "Show ambient / _color / _minlight in the light preview", g_krPreviewAmbient,
	                                 "Off: the live light preview (Ctrl+L) leaves them out, so you see the lights and the sun alone." ) );
	previews->addWidget( PreviewBox( panel, "Show _floodlight in the light preview (approximate)", g_krPreviewFlood,
	                                 "The preview adds the flood light by how much sky a spot sees - close to q3map2 outdoors; "
	                                 "q3map2 also counts walls further away than the distance as open, so rooms come out a little "
	                                 "brighter after compiling." ) );
	lg->addLayout( previews, r++, 0, 1, 5 );
	auto *lnote = new QLabel( "This light shows in the live light preview (Ctrl+L) - compile to see the real thing." );
	lnote->setWordWrap( true );
	lnote->setStyleSheet( "color: gray;" );
	lg->addWidget( lnote, r++, 0, 1, 5 );
	lg->setColumnStretch( 1, 1 );
	return light;
}

QGroupBox* GameSection( Panel& panel ){
	auto *view = new QGroupBox( "In game" );
	auto *vg = new QGridLayout( view );
	int r = 0;
	NumberRow( panel, vg, r, "distancecull", { "distancecull" }, 0, 32768, 64, 0,
	           "The game draws nothing further away than this (and VIS throws away what is beyond - q3map2 and sof2map). "
	           "With fog, put it where the fog is nearly full, so the cut can't be seen." );
	vg->addWidget( PreviewBox( panel, "Cut the 3D view at distancecull, like the game", g_krPreviewDistanceCull,
	                           "The editor's camera stops drawing at that distance. With the map's fog on (Fog window) the "
	                           "edge disappears into the fog the way it does in game." ), r++, 1, 1, 4 );
	{
		vg->addWidget( new QLabel( "fog" ), r, 0 );
		auto *name = new QLabel;
		vg->addWidget( name, r, 1, 1, 2 );
		auto *b = new QPushButton( "Fog..." );
		b->setToolTip( "The Fog window: show the map's fog in the 3D view, try other fogs." );
		QObject::connect( b, &QPushButton::clicked, [](){ GlobalCommands_find( "KrFogWindow" ).m_callback(); } );
		vg->addWidget( b, r, 3 );
		vg->addWidget( ResetButton( panel, { "fog" } ), r, 4 );
		panel.readers.push_back( [name](){
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
		vg->addWidget( e, r, 1, 1, 3 );
		vg->addWidget( ResetButton( panel, { "gridsize" } ), r, 4 );
		QObject::connect( e, &QLineEdit::editingFinished, [e](){
			Key_set( "gridsize", e->text().trimmed().toLatin1().constData() );
		} );
		panel.readers.push_back( [e](){
			if( e->hasFocus() )
				return;
			Entity* w = Worldspawn();
			e->setText( w != nullptr ? w->getKeyValue( "gridsize" ) : "" );
		} );
		++r;
	}
	vg->setColumnStretch( 1, 1 );
	return view;
}

/// typing in a value cell changes the key at once; Enter keeps it (one undo step), Esc puts it back
class LiveDelegate : public QStyledItemDelegate
{
public:
	int* m_editing; // editors open: the table is not rebuilt under them
	LiveDelegate( QObject* parent, int* editing ) : QStyledItemDelegate( parent ), m_editing( editing ){}
	static CopiedString keyOf( const QModelIndex& index ){
		return index.sibling( index.row(), 0 ).data( Qt::UserRole ).toString().toLatin1().constData();
	}
	QWidget* createEditor( QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index ) const override {
		QWidget* w = QStyledItemDelegate::createEditor( parent, option, index );
		++*m_editing;
		if( auto *e = qobject_cast<QLineEdit*>( w ) ){
			const CopiedString key = keyOf( index );
			QObject::connect( e, &QLineEdit::textEdited, [key]( const QString& t ){ Key_live( key.c_str(), t.toLatin1().constData() ); } );
		}
		return w;
	}
	void setModelData( QWidget* editor, QAbstractItemModel* model, const QModelIndex& index ) const override {
		if( auto *e = qobject_cast<QLineEdit*>( editor ) ){
			Key_live( keyOf( index ).c_str(), e->text().toLatin1().constData() );
			Keys_commit();
		}
		QStyledItemDelegate::setModelData( editor, model, index );
	}
	void destroyEditor( QWidget* editor, const QModelIndex& index ) const override {
		Keys_revert(); // nothing left when it was committed
		--*m_editing;
		QStyledItemDelegate::destroyEditor( editor, index );
	}
};

QGroupBox* KeysSection( Panel& panel ){
	auto *group = new QGroupBox( "All worldspawn keys" );
	auto *v = new QVBoxLayout( group );
	auto *table = new QTableWidget( 0, 3 );
	table->setHorizontalHeaderLabels( { "Key", "Value", "When loaded" } );
	table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
	table->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
	table->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::Interactive );
	table->verticalHeader()->hide();
	table->setSelectionBehavior( QAbstractItemView::SelectRows );
	table->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed );
	table->setMinimumHeight( 170 );
	table->setToolTip( "Double-click a value to edit it - the map changes while you type, Enter keeps it, Esc puts it back.\n"
	                   "Bold = changed since the map was loaded; italic = removed since." );
	auto editing = std::make_shared<int>( 0 );
	table->setItemDelegateForColumn( 1, new LiveDelegate( table, editing.get() ) );
	v->addWidget( table );

	auto *buttons = new QHBoxLayout;
	auto *reset = new QPushButton( "Reset selected" );
	reset->setToolTip( "The selected keys back to how they were when the map was loaded" );
	auto *remove = new QPushButton( "Remove selected" );
	buttons->addWidget( reset );
	buttons->addWidget( remove );
	buttons->addStretch( 1 );
	v->addLayout( buttons );
	auto *add = new QHBoxLayout;
	auto *newKey = new QLineEdit;
	newKey->setPlaceholderText( "key" );
	auto *newValue = new QLineEdit;
	newValue->setPlaceholderText( "value" );
	auto *set = new QPushButton( "Add / Set" );
	add->addWidget( newKey, 1 );
	add->addWidget( newValue, 2 );
	add->addWidget( set );
	v->addLayout( add );

	const auto selectedKeys = [table](){
		std::vector<CopiedString> keys;
		for( const QModelIndex& i : table->selectionModel()->selectedRows( 0 ) )
			keys.emplace_back( i.data( Qt::UserRole ).toString().toLatin1().constData() );
		return keys;
	};
	QObject::connect( reset, &QPushButton::clicked, [selectedKeys](){ Keys_reset( selectedKeys() ); } );
	QObject::connect( remove, &QPushButton::clicked, [selectedKeys](){
		Keys_commit();
		for( const CopiedString& k : selectedKeys() )
			Key_live( k.c_str(), "" );
		Keys_commit( "worldspawnRemoveKeys" );
	} );
	const auto doSet = [newKey, newValue](){
		const QByteArray k = newKey->text().trimmed().toLatin1();
		if( k.isEmpty() || string_equal_nocase( k.constData(), "classname" ) )
			return;
		Key_set( k.constData(), newValue->text().toLatin1().constData() );
		newKey->clear();
		newValue->clear();
		newKey->setFocus();
	};
	QObject::connect( set, &QPushButton::clicked, doSet );
	QObject::connect( newValue, &QLineEdit::returnPressed, doSet );

	auto shown = std::make_shared<QString>();
	panel.readers.push_back( [table, shown, editing](){
		if( *editing > 0 )
			return;
		const Entity* w = Worldspawn();
		struct Row { CopiedString key, value, was; bool has, had; };
		std::vector<Row> rows;
		for( const auto& [k, val] : Keys_of( w ) ){
			bool had;
			const char* was = g_snapshot.get( k.c_str(), had );
			rows.push_back( { k, val, was, true, had } );
		}
		for( const auto& [k, was] : g_snapshot.keys ){
			bool there = false;
			for( const Row& r : rows )
				there = there || string_equal_nocase( r.key.c_str(), k.c_str() );
			if( !there )
				rows.push_back( { k, "", was, false, true } );
		}
		QString sig;
		for( const Row& r : rows )
			sig += QString( "%1\x1f%2\x1f%3\x1f%4%5\x1e" ).arg( r.key.c_str(), r.value.c_str(), r.was.c_str() ).arg( r.has ).arg( r.had );
		if( sig == *shown )
			return;
		*shown = sig;
		const int keepRow = table->currentRow();
		table->setRowCount( int( rows.size() ) );
		for( int i = 0; i < int( rows.size() ); ++i ){
			const Row& r = rows[i];
			const bool classname = string_equal_nocase( r.key.c_str(), "classname" );
			const bool changed = !r.has || !r.had || !string_equal( r.value.c_str(), r.was.c_str() );
			auto *k = new QTableWidgetItem( r.key.c_str() );
			k->setData( Qt::UserRole, QString( r.key.c_str() ) );
			k->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
			QFont f = k->font();
			f.setBold( changed );
			f.setItalic( !r.has );
			k->setFont( f );
			auto *val = new QTableWidgetItem( r.has ? QString( r.value.c_str() ) : QString() );
			val->setFlags( classname ? Qt::ItemIsSelectable | Qt::ItemIsEnabled : Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable );
			if( !r.has )
				val->setToolTip( "Removed since the map was loaded - type a value to set it again, or Reset" );
			auto *was = new QTableWidgetItem( r.had ? QString( r.was.c_str() ) : QString( "(new)" ) );
			was->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
			was->setForeground( QColor( 128, 128, 128 ) );
			table->setItem( i, 0, k );
			table->setItem( i, 1, val );
			table->setItem( i, 2, was );
		}
		if( keepRow >= 0 && keepRow < table->rowCount() )
			table->setCurrentCell( keepRow, 1, QItemSelectionModel::NoUpdate );
	} );
	return group;
}

/* ---- the Worldspawn tab ---- */
QWidget* g_tab = nullptr;     // the scroll area in the Textures / Models / Entities tabs ...
QWidget* g_page = nullptr;    // ... or the group window page

QWidget* WorldspawnTab_construct(){
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable( true );
	auto *inner = new QWidget;
	Panel& panel = Panel_new( scroll );
	auto *v = new QVBoxLayout( inner );

	auto *top = new QHBoxLayout;
	auto *noWorld = new QLabel( "No map open." );
	top->addWidget( noWorld );
	auto *info = new QLabel;
	info->setStyleSheet( "color: gray;" );
	top->addWidget( info, 1 );
	auto *resetAll = new QPushButton( "Reset all" );
	resetAll->setToolTip( "Every worldspawn key back to how it was when the map was loaded (one undo step)" );
	QObject::connect( resetAll, &QPushButton::clicked, [](){ Keys_resetAll(); } );
	top->addWidget( resetAll );
	auto *mark = new QPushButton( "Keep as reset point" );
	mark->setToolTip( "From now on Reset goes back to the keys as they are now (instead of as loaded)" );
	QObject::connect( mark, &QPushButton::clicked, [](){ Snapshot_take(); Panels_read(); } );
	top->addWidget( mark );
	v->addLayout( top );
	panel.readers.push_back( [noWorld, info, resetAll](){
		const Entity* w = Worldspawn();
		noWorld->setVisible( w == nullptr );
		int changed = 0;
		for( const auto& [k, val] : Keys_of( w ) )
			changed += Key_changed( k.c_str() );
		for( const auto& [k, was] : g_snapshot.keys )
			if( w != nullptr && string_empty( w->getKeyValue( k.c_str() ) ) && !string_empty( was.c_str() ) )
				++changed;
		info->setText( changed == 0 ? QString( "Changes show at once in the views." )
		               : QString( "%1 key%2 changed since the map was loaded." ).arg( changed ).arg( changed == 1 ? "" : "s" ) );
		resetAll->setEnabled( changed != 0 );
	} );

	v->addWidget( KeysSection( panel ) );
	v->addWidget( LightSection( panel, "Light - used when you compile (LIGHT stage)" ) );
	v->addWidget( GameSection( panel ) );
	v->addStretch( 1 );
	scroll->setWidget( inner );
	return scroll;
}

void MapSettings_show(){
	if( g_tab != nullptr && g_texModelTabs != nullptr ){
		const int i = g_texModelTabs->indexOf( g_tab );
		if( i >= 0 ){
			g_texModelTabs->setCurrentIndex( i );
			return;
		}
	}
	if( g_page != nullptr )
		GroupDialog_showPage( g_page );
}

} // namespace

/* ---- for the main window ---- */

/// the "Worldspawn" tab next to Textures / Models / Entities
QWidget* KrWorldspawn_constructTab(){
	g_tab = WorldspawnTab_construct();
	return g_tab;
}
/// the layouts without those tabs: a page of the group window
void KrWorldspawn_constructPage(){
	g_page = GroupDialog_addPage( "Worldspawn", WorldspawnTab_construct(), RawStringExportCaller( "Worldspawn" ) );
}
/// the light keys for the Sun Calibrator
QWidget* KrMapSettings_lightSection(){
	auto *holder = new QWidget;
	auto *v = new QVBoxLayout( holder );
	v->setContentsMargins( 0, 0, 0, 0 );
	Panel& panel = Panel_new( holder );
	v->addWidget( LightSection( panel, "Worldspawn light (ambient, _color, _minlight, _floodlight) - also in the Worldspawn tab" ) );
	return holder;
}
/// a map was loaded or made: what Reset goes back to
void KrWorldspawn_snapshot(){
	Snapshot_take();
}

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
