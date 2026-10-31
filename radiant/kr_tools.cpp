/*
   KenRadiant tools:
   - Brush Shapes: Unreal-style geometry presets (box, cone, cylinder, curved / linear / spiral stairs, sphere) with live update
   - Check for Problems (like Hammer's Alt+P): broken targets, missing models / textures, invalid / duplicate brushes ...

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include "gtkutil/guisettings.h"
#include "kr_brushshapes.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QStackedWidget>
#include <QLabel>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>

#include <functional>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

#include "iundo.h"
#include "iselection.h"
#include "ientity.h"
#include "ishaders.h"
#include "ifilesystem.h"
#include "scenelib.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "eclasslib.h"
#include "mainframe.h"
#include "texwindow.h"
#include "grid.h"
#include "brush.h"
#include "brushnode.h"
#include "patch.h"
#include "filterbar.h"

Vector3 KrCamera_targetPoint();
void GlobalCamera_FocusOnSelected();

namespace
{

/// slider + spin box editing one number
class KrField
{
public:
	QSlider* slider;
	QDoubleSpinBox* spin;
	float scale;
	std::function<void()> changed;
	KrField( float lo, float hi, float step, int decimals ) : scale( 1.f / step ){
		slider = new QSlider( Qt::Horizontal );
		slider->setRange( int( std::lround( lo * scale ) ), int( std::lround( hi * scale ) ) );
		slider->setMinimumWidth( 160 );
		spin = new QDoubleSpinBox;
		spin->setRange( lo, hi );
		spin->setSingleStep( step );
		spin->setDecimals( decimals );
		spin->setKeyboardTracking( false );
		QObject::connect( slider, &QSlider::valueChanged, [this]( int v ){
			const QSignalBlocker b( spin );
			spin->setValue( v / scale );
			if( changed ) changed();
		} );
		QObject::connect( spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [this]( double v ){
			const QSignalBlocker b( slider );
			slider->setValue( int( std::lround( v * scale ) ) );
			if( changed ) changed();
		} );
	}
	float value() const { return float( spin->value() ); }
	void set( float v ){
		const QSignalBlocker b1( slider ), b2( spin );
		spin->setValue( v );
		slider->setValue( int( std::lround( v * scale ) ) );
	}
};

/* ---------------- Brush Shapes ---------------- */

class BrushShapesDialog : public QDialog
{
	KrBrushShape m_shape;
	QComboBox* m_type;
	QStackedWidget* m_pages;
	QCheckBox* m_live;
	QLabel* m_info;
	std::vector<std::unique_ptr<KrField>> m_fields;
	std::vector<QCheckBox*> m_checks;
	// live update: the last created shape is replaced while nothing else was done since
	std::size_t m_undoSize = std::size_t( -1 );
	Vector3 m_origin{ 0, 0, 0 };

	QGridLayout* newPage(){
		auto *page = new QWidget;
		auto *grid = new QGridLayout( page );
		grid->setColumnStretch( 1, 1 );
		m_pages->addWidget( page );
		return grid;
	}
	void addFloat( QGridLayout* grid, const char* label, float& value, float lo, float hi, float step, const char* tip ){
		auto field = std::make_unique<KrField>( lo, hi, step, step < 1 ? 2 : 0 );
		field->set( value );
		KrField* f = field.get();
		field->changed = [this, f, &value](){ value = f->value(); edited(); };
		auto *l = new QLabel( label );
		l->setToolTip( tip );
		f->slider->setToolTip( tip );
		f->spin->setToolTip( tip );
		const int r = grid->rowCount();
		grid->addWidget( l, r, 0 );
		grid->addWidget( f->slider, r, 1 );
		grid->addWidget( f->spin, r, 2 );
		m_fields.push_back( std::move( field ) );
	}
	void addInt( QGridLayout* grid, const char* label, int& value, int lo, int hi, const char* tip ){
		auto field = std::make_unique<KrField>( float( lo ), float( hi ), 1, 0 );
		field->set( float( value ) );
		KrField* f = field.get();
		field->changed = [this, f, &value](){ value = int( std::lround( f->value() ) ); edited(); };
		auto *l = new QLabel( label );
		l->setToolTip( tip );
		const int r = grid->rowCount();
		grid->addWidget( l, r, 0 );
		grid->addWidget( f->slider, r, 1 );
		grid->addWidget( f->spin, r, 2 );
		m_fields.push_back( std::move( field ) );
	}
	void addBool( QGridLayout* grid, const char* label, bool& value, const char* tip ){
		auto *c = new QCheckBox( label );
		c->setChecked( value );
		c->setToolTip( tip );
		QObject::connect( c, &QCheckBox::toggled, [this, &value]( bool on ){ value = on; edited(); } );
		grid->addWidget( c, grid->rowCount(), 0, 1, 3 );
		m_checks.push_back( c );
	}

	void build( const Vector3& origin ){
		const char* selected = TextureBrowser_GetSelectedShader();
		// nothing picked in the texture browser yet: the game's caulk instead of "shader not found"
		const CopiedString caulk = GetCommonShader( "caulk" );
		const char* shader = string_empty( selected ) || string_equal( selected, "textures/" ) ? caulk.c_str() : selected;
		UndoableCommand undo( "brushShape" );
		GlobalSelectionSystem().setSelectedAll( false );
		const std::size_t count = KrBrushShape_build( m_shape, origin, shader );
		m_undoSize = GlobalUndoSystem().size(); // the step is on the stack as soon as the first brush saved its state
		m_origin = origin;
		m_info->setText( QString( "%1 brush%2 with %3. Changes update it live until you edit something else." )
		                 .arg( count ).arg( count == 1 ? "" : "es" ).arg( shader ) );
	}
	void edited(){
		if( !m_live->isChecked() || m_undoSize == std::size_t( -1 ) || GlobalUndoSystem().size() != m_undoSize )
			return;
		GlobalUndoSystem().undo(); // remove the previous version, rebuild in one undo step
		build( m_origin );
	}

public:
	BrushShapesDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ){
		setWindowTitle( "Brush Shapes" );
		auto *vbox = new QVBoxLayout( this );
		m_type = new QComboBox;
		m_type->addItems( { "Box", "Cone", "Cylinder", "Curved Stair", "Linear Stair", "Spiral Stair", "Sphere" } );
		vbox->addWidget( m_type );
		m_pages = new QStackedWidget;
		vbox->addWidget( m_pages );
		KrBrushShape& s = m_shape;
		{ // box
			auto *g = newPage();
			addFloat( g, "X", s.x, 8, 4096, 8, "Size along X" );
			addFloat( g, "Y", s.y, 8, 4096, 8, "Size along Y" );
			addFloat( g, "Z", s.z, 8, 4096, 8, "Height" );
			addBool( g, "Hollow", s.hollow, "Six wall brushes around an empty room" );
			addFloat( g, "Wall thickness", s.wall, 1, 256, 1, "Thickness of the walls when hollow" );
		}
		{ // cone
			auto *g = newPage();
			addFloat( g, "Z", s.height, 8, 4096, 8, "Height of the cone" );
			addFloat( g, "Cap Z", s.capHeight, 8, 4096, 8, "Height of the inner cone when hollow" );
			addFloat( g, "Outer radius", s.outerRadius, 8, 4096, 8, "" );
			addFloat( g, "Inner radius", s.innerRadius, 4, 4096, 4, "Inner radius when hollow" );
			addInt( g, "Sides", s.sides, 3, 64, "" );
			addBool( g, "Hollow", s.hollow, "One brush per side, between the outer and the inner cone" );
		}
		{ // cylinder
			auto *g = newPage();
			addFloat( g, "Z", s.height, 8, 4096, 8, "Height" );
			addFloat( g, "Outer radius", s.outerRadius, 8, 4096, 8, "" );
			addFloat( g, "Inner radius", s.innerRadius, 4, 4096, 4, "Inner radius when hollow" );
			addInt( g, "Sides", s.sides, 3, 64, "" );
			addBool( g, "Hollow", s.hollow, "A ring of brushes (pipe / tower)" );
		}
		{ // curved stair
			auto *g = newPage();
			addFloat( g, "Inner radius", s.stairInnerRadius, 0, 4096, 8, "" );
			addFloat( g, "Step height", s.stepHeight, 1, 256, 1, "" );
			addFloat( g, "Step width", s.stepWidth, 8, 2048, 8, "From the inner to the outer radius" );
			addFloat( g, "Angle of curve", s.curveAngle, 5, 360, 5, "Degrees the stair turns" );
			addInt( g, "Steps", s.numSteps, 1, 128, "" );
			addFloat( g, "Add to first step", s.addToFirstStep, 0, 1024, 1, "Extra height under every step" );
			addBool( g, "Counter clockwise", s.counterClockwise, "" );
		}
		{ // linear stair
			auto *g = newPage();
			addFloat( g, "Step length", s.stepLength, 1, 1024, 1, "Depth of one step (along X)" );
			addFloat( g, "Step height", s.stepHeight, 1, 256, 1, "" );
			addFloat( g, "Step width", s.stepWidth, 8, 4096, 8, "Along Y" );
			addInt( g, "Steps", s.numSteps, 1, 128, "" );
			addFloat( g, "Add to first step", s.addToFirstStep, 0, 1024, 1, "Extra height under every step" );
		}
		{ // spiral stair
			auto *g = newPage();
			addFloat( g, "Inner radius", s.stairInnerRadius, 0, 4096, 8, "" );
			addFloat( g, "Step width", s.stepWidth, 8, 2048, 8, "" );
			addFloat( g, "Step height", s.stepHeight, 1, 256, 1, "" );
			addFloat( g, "Step thickness", s.stepThickness, 1, 256, 1, "" );
			addInt( g, "Steps per 360", s.stepsPer360, 3, 64, "" );
			addInt( g, "Steps", s.numSteps, 1, 256, "" );
			addBool( g, "Sloped ceiling", s.slopedCeiling, "The underside of the stair is a smooth ramp" );
			addBool( g, "Sloped floor", s.slopedFloor, "The walking surface is a smooth ramp" );
			addBool( g, "Counter clockwise", s.counterClockwise, "" );
		}
		{ // sphere
			auto *g = newPage();
			addFloat( g, "Radius", s.sphereRadius, 8, 4096, 8, "" );
			addInt( g, "Tessellation", s.tessellation, 0, 3, "0 = icosahedron (20 faces) ... 3 = 1280 faces" );
		}
		QObject::connect( m_type, QOverload<int>::of( &QComboBox::currentIndexChanged ), [this]( int i ){
			m_pages->setCurrentIndex( i );
			m_shape.type = i;
			edited();
		} );
		m_live = new QCheckBox( "Live update the last created shape" );
		m_live->setChecked( true );
		vbox->addWidget( m_live );
		m_info = new QLabel( "Creates the shape on the surface in the middle of the 3D view, with the selected texture." );
		m_info->setWordWrap( true );
		vbox->addWidget( m_info );
		auto *buttons = new QHBoxLayout;
		auto *create = new QPushButton( "Create" );
		create->setDefault( true );
		QObject::connect( create, &QPushButton::clicked, [this](){ build( KrCamera_targetPoint() ); } );
		auto *close = new QPushButton( "Close" );
		QObject::connect( close, &QPushButton::clicked, [this](){ hide(); } );
		buttons->addStretch();
		buttons->addWidget( create );
		buttons->addWidget( close );
		vbox->addLayout( buttons );
	}
};

BrushShapesDialog* g_brushShapes = nullptr;

/* ---------------- Check for Problems ---------------- */

struct KrProblem
{
	QString kind, text;
	scene::Path path;
};

class ProblemWalker : public scene::Graph::Walker
{
public:
	std::vector<KrProblem>& m_problems;
	std::set<CopiedString> m_targetnames;
	struct Target{ CopiedString key, value; scene::Path path; };
	mutable std::vector<Target> m_targets;
	mutable std::map<std::string, scene::Path> m_brushPlanes, m_entities;
	mutable std::map<std::string, scene::Path> m_missingTex;
	mutable int m_playerStarts = 0;
	// engine limit counts
	mutable int m_gameEntities = 0, m_inlineModels = 0, m_brushes = 0;
	mutable std::set<std::string> m_shaders, m_sounds, m_models;
	ProblemWalker( std::vector<KrProblem>& problems ) : m_problems( problems ){}

	void add( const char* kind, const QString& text, const scene::Path& path ) const {
		m_problems.push_back( { kind, text, path } );
	}
	static bool modelExists( const char* model ){
		if( string_empty( model ) || model[0] == '*' )
			return true;
		return !string_empty( GlobalFileSystem().findFile( model ) );
	}
	void checkShader( const char* name, const scene::Path& path ) const {
		if( string_empty( name ) || m_missingTex.count( name ) )
			return;
		IShader* shader = GlobalShaderSystem().getShaderForName( name );
		const bool missing = shader != nullptr && shader->krNotFound();
		if( shader != nullptr )
			shader->DecRef();
		if( missing ){
			m_missingTex.emplace( name, path );
			add( "Missing texture", QString( "%1: no shader image or texture file found" ).arg( name ), path );
		}
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		scene::Node& node = path.top();
		if( Entity* entity = Node_getEntity( node ) ){
			const char* classname = entity->getClassName();
			if( string_equal_prefix( classname, "info_player" ) )
				++m_playerStarts;
			// what is still an entity in the game (q3map2 bakes / removes these)
			static const char* const compiledAway[] = { "worldspawn", "func_group", "misc_model", "misc_gamemodel", "light", "info_null", "_decal", "_skybox", "misc_terrain" };
			const bool gone = std::any_of( std::begin( compiledAway ), std::end( compiledAway ), [classname]( const char* c ){ return string_equal_nocase( classname, c ); } );
			if( !gone ){
				++m_gameEntities;
				if( entity->isContainer() )
					++m_inlineModels;
				const char* model = entity->getKeyValue( "model" );
				if( !string_empty( model ) && model[0] != '*' )
					m_models.insert( model );
			}
			for( const char* key : { "noise", "sound", "soundSet", "music" } )
				if( const char* v = entity->getKeyValue( key ); !string_empty( v ) )
					m_sounds.insert( v );
			if( entity->getEntityClass().unknown && !string_equal( classname, "worldspawn" ) )
				add( "Unknown entity", QString( "%1 is not defined in the game's entity definitions" ).arg( classname ), path );
			for( const char* key : { "target", "target2", "target3", "target4", "killtarget" } ){
				const char* value = entity->getKeyValue( key );
				if( !string_empty( value ) )
					m_targets.push_back( { key, value, path } );
			}
			const char* model = entity->getKeyValue( "model" );
			if( !modelExists( model ) )
				add( "Missing model", QString( "%1: model %2 not found" ).arg( classname, model ), path );
			{
				// duplicate point entities: same class, origin, angles and model
				const auto key = StringStream<256>( classname, '|', entity->getKeyValue( "origin" ), '|', entity->getKeyValue( "angles" ), entity->getKeyValue( "angle" ), '|', model );
				if( !entity->isContainer() && !string_empty( entity->getKeyValue( "origin" ) ) ){
					if( m_entities.count( key.c_str() ) )
						add( "Duplicate", QString( "%1 at %2 is there twice" ).arg( classname, entity->getKeyValue( "origin" ) ), path );
					else
						m_entities.emplace( key.c_str(), path );
				}
			}
			if( entity->isContainer() && !string_equal( classname, "worldspawn" ) && Node_getTraversable( node ) != nullptr ){
				bool hasChild = false;
				class Any : public scene::Traversable::Walker { bool& m_any; public: Any( bool& any ) : m_any( any ){} bool pre( scene::Node& ) const override { m_any = true; return false; } };
				Node_getTraversable( node )->traverse( Any( hasChild ) );
				if( !hasChild && string_empty( model ) )
					add( "Empty entity", QString( "%1 has no brushes" ).arg( classname ), path );
			}
		}
		else if( Brush* brush = Node_getBrush( node ) ){
			++m_brushes;
			for( const auto& face : *brush )
				m_shaders.insert( face->GetShader() );
			std::size_t faces = 0;
			std::string key;
			for( const auto& face : *brush ){
				if( face->contributes() )
					++faces;
				checkShader( face->GetShader(), path );
				const Plane3& p = face->getPlane().plane3();
				key += StringStream<96>( int( std::lround( p.a * 1000 ) ), ' ', int( std::lround( p.b * 1000 ) ), ' ', int( std::lround( p.c * 1000 ) ), ' ', int( std::lround( p.d * 10 ) ), ';' ).c_str();
			}
			if( faces < 4 )
				add( "Invalid brush", QString( "brush with only %1 real faces (degenerate, it will be dropped)" ).arg( faces ), path );
			else{
				// same planes in the same order: an exact copy on top of another brush
				if( m_brushPlanes.count( key ) )
					add( "Duplicate", "brush exactly on top of another one", path );
				else
					m_brushPlanes.emplace( key, path );
			}
		}
		else if( Patch* patch = Node_getPatch( node ) ){
			m_shaders.insert( patch->GetShader() );
			checkShader( patch->GetShader(), path );
		}
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {}

	void finish( const std::set<CopiedString>& targetnames ) const {
		for( const Target& t : m_targets )
			if( !targetnames.count( t.value ) )
				add( "Broken target", QString( "%1 \"%2\": no entity has this targetname" ).arg( t.key.c_str(), t.value.c_str() ), t.path );
		if( m_playerStarts == 0 )
			add( "No player start", "no info_player_* entity: the map can't be played", scene::Path() );
		// SOF2MP's Quake 3 engine limits (sof2vbo / other engines may allow more); warn well before, the game also spawns entities
		const auto limit = [this]( const char* what, std::size_t count, std::size_t warn, std::size_t max, const char* why ){
			if( count >= warn )
				add( count >= max ? "Engine limit!" : "Engine limit", QString( "%1: %2 of %3 - %4" ).arg( what ).arg( count ).arg( max ).arg( why ), scene::Path() );
		};
		limit( "Game entities", m_gameEntities, 800, 960, "the game has 1024 entity slots and needs ~64 for players plus room for weapons, effects and items" );
		limit( "Brush entities (inline models)", m_inlineModels, 200, 255, "doors, movers, triggers ...: 256 inline models in a map, model 0 is the world" );
		limit( "Different models on entities", m_models.size(), 160, 256, "the game's model list (MAX_MODELS 256) is shared with weapons and players" );
		limit( "Different sounds on entities", m_sounds.size(), 128, 256, "the game's sound list (MAX_SOUNDS 256) is shared with weapons and players" );
		limit( "Different shaders", m_shaders.size(), 900, 1024, "q3map2 stops at 1024 shaders in a .bsp (MAX_MAP_SHADERS)" );
		limit( "Brushes", std::size_t( m_brushes ), 28000, 32768, "q3map2's brush limit (MAX_MAP_BRUSHES)" );
		globalOutputStream() << "Check for Problems: " << m_gameEntities << " game entities, " << m_inlineModels << " brush entities, " << int( m_models.size() ) << " models, "
		                     << int( m_sounds.size() ) << " sounds, " << int( m_shaders.size() ) << " shaders, " << m_brushes << " brushes\n";
	}
};

class TargetnameWalker : public scene::Graph::Walker
{
public:
	std::set<CopiedString>& m_names;
	TargetnameWalker( std::set<CopiedString>& names ) : m_names( names ){}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( Entity* entity = Node_getEntity( path.top() ) ){
			const char* name = entity->getKeyValue( "targetname" );
			if( !string_empty( name ) )
				m_names.insert( name );
			return true;
		}
		return true;
	}
};

class ProblemsDialog : public QDialog
{
	QTreeWidget* m_tree;
	QLabel* m_summary;
	std::vector<KrProblem> m_problems;
public:
	ProblemsDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ){
		setWindowTitle( "Check for Problems" );
		resize( 900, 520 );
		auto *vbox = new QVBoxLayout( this );
		m_summary = new QLabel;
		vbox->addWidget( m_summary );
		m_tree = new QTreeWidget;
		m_tree->setColumnCount( 2 );
		m_tree->setHeaderLabels( { "Problem", "Details" } );
		m_tree->setRootIsDecorated( true ); // grouped by kind
		m_tree->header()->setStretchLastSection( true );
		m_tree->setSelectionMode( QAbstractItemView::ExtendedSelection );
		vbox->addWidget( m_tree );
		QObject::connect( m_tree, &QTreeWidget::itemSelectionChanged, [this](){ selectInMap( false ); } );
		QObject::connect( m_tree, &QTreeWidget::itemDoubleClicked, [this](){ selectInMap( true ); } );
		auto *buttons = new QHBoxLayout;
		auto *go = new QPushButton( "Go to" );
		go->setToolTip( "Select it in the map and fly the camera there (or double-click the line)" );
		QObject::connect( go, &QPushButton::clicked, [this](){ selectInMap( true ); } );
		auto *refresh = new QPushButton( "Check again" );
		QObject::connect( refresh, &QPushButton::clicked, [this](){ check(); } );
		auto *close = new QPushButton( "Close" );
		QObject::connect( close, &QPushButton::clicked, [this](){ hide(); } );
		buttons->addWidget( go );
		buttons->addStretch();
		buttons->addWidget( refresh );
		buttons->addWidget( close );
		vbox->addLayout( buttons );
	}
	void check(){
		m_problems.clear();
		m_tree->clear();
		std::set<CopiedString> targetnames;
		GlobalSceneGraph().traverse( TargetnameWalker( targetnames ) );
		ProblemWalker walker( m_problems );
		GlobalSceneGraph().traverse( walker );
		walker.finish( targetnames );
		// one group per kind ("Missing texture (13)"), in the order they were found; small groups open
		std::vector<std::pair<QString, QTreeWidgetItem*>> groups;
		for( std::size_t i = 0; i < m_problems.size(); ++i ){
			QTreeWidgetItem* group = nullptr;
			for( auto& [kind, item] : groups )
				if( kind == m_problems[i].kind )
					group = item;
			if( group == nullptr ){
				group = new QTreeWidgetItem( { m_problems[i].kind, QString() } );
				group->setData( 0, Qt::UserRole, -1 );
				QFont f = group->font( 0 );
				f.setBold( true );
				group->setFont( 0, f );
				m_tree->addTopLevelItem( group );
				groups.emplace_back( m_problems[i].kind, group );
			}
			auto *item = new QTreeWidgetItem( group, { m_problems[i].kind, m_problems[i].text } );
			item->setData( 0, Qt::UserRole, int( i ) );
		}
		for( auto& [kind, group] : groups ){
			const int n = group->childCount();
			group->setText( 0, QString( "%1 (%2)" ).arg( kind ).arg( n ) );
			group->setText( 1, n == 1 ? group->child( 0 )->text( 1 ) : QString( "click the arrow to see them - selecting the group selects all %1" ).arg( n ) );
			group->setExpanded( n <= 12 || m_problems.size() <= 40 );
		}
		m_tree->resizeColumnToContents( 0 );
		m_summary->setText( m_problems.empty() ? "No problems found." : QString( "%1 problem%2 - click one to select it, double-click to fly there." )
		                    .arg( m_problems.size() ).arg( m_problems.size() == 1 ? "" : "s" ) );
		globalOutputStream() << "Check for Problems: " << int( m_problems.size() ) << " found\n";
	}
	void selectInMap( bool focus ){
		const auto items = m_tree->selectedItems();
		if( items.isEmpty() )
			return;
		if( GlobalSelectionSystem().Mode() != SelectionSystem::ePrimitive )
			GlobalSelectionSystem().SetMode( SelectionSystem::ePrimitive );
		GlobalSelectionSystem().setSelectedAll( false );
		bool any = false;
		const auto select = [this, &any]( const QTreeWidgetItem* item ){
			const int i = item->data( 0, Qt::UserRole ).toInt();
			if( i < 0 || i >= int( m_problems.size() ) )
				return;
			const KrProblem& p = m_problems[i];
			if( p.path.size() < 2 )
				return;
			if( scene::Instance* instance = GlobalSceneGraph().find( p.path ) ){
				Instance_setSelected( *instance, true );
				any = true;
			}
		};
		for( const QTreeWidgetItem* item : items ){
			select( item );
			for( int c = 0; c < item->childCount(); ++c ) // a group: all of its kind
				select( item->child( c ) );
		}
		if( any && focus )
			GlobalCamera_FocusOnSelected();
	}
};

ProblemsDialog* g_problems = nullptr;

} // namespace

void KrBrushShapes_show(){
	if( g_brushShapes == nullptr ){
		g_brushShapes = new BrushShapesDialog( MainFrame_getWindow() );
		g_guiSettings.addWindow( g_brushShapes, "KenRadiant/BrushShapes" ); // where you left it, next time too
	}
	g_brushShapes->show();
	g_brushShapes->raise();
	g_brushShapes->activateWindow();
}

void KrCheckProblems_show(){
	if( g_problems == nullptr ){
		g_problems = new ProblemsDialog( MainFrame_getWindow() );
		g_guiSettings.addWindow( g_problems, "KenRadiant/Problems" ); // where you left it, next time too
	}
	g_problems->check();
	g_problems->show();
	g_problems->raise();
	g_problems->activateWindow();
}
