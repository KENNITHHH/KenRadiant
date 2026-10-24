/*
   KenRadiant: Entities tab (next to Textures / Models) - every point entity of the game, its keys, spawnflags and
   (SOF2) the gametypes it appears in; set them up, then drag the entity into the 2D or 3D view to place it there
   (double-click: in the middle of the 3D view).

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <map>
#include <vector>
#include <algorithm>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QTreeWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QCheckBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QTimer>

#include "ieclass.h"
#include "eclasslib.h"
#include "ientity.h"
#include "iselection.h"
#include "iundo.h"
#include "qerplugin.h"
#include "string/string.h"
#include "stream/stringstream.h"
#include "mainframe.h"
#include "entity.h"
#include "xywindow.h"
#include "camwindow.h"
#include "grid.h"

bool KrCamera_pointAt( int x, int y, Vector3& point );
Vector3 KrCamera_targetPoint();
Vector3 KrCamera_origin();
void XYWnd_MouseToPoint( XYWnd* xywnd, int x, int y, Vector3& point );

namespace
{
const char* const c_mime = "application/x-kenradiant-entity";

class KeepKeysFilter : public QObject
{
protected:
	bool eventFilter( QObject* obj, QEvent* event ) override {
		if( event->type() == QEvent::ShortcutOverride ){
			auto *key = static_cast<QKeyEvent*>( event );
			if( key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::ShiftModifier || key->modifiers() == Qt::KeypadModifier )
				event->accept();
		}
		return QObject::eventFilter( obj, event );
	}
};

/// what the user set up per class: keys (empty = not written), spawnflags
struct Setup
{
	std::map<std::string, std::string> keys;
	int spawnflags = 0;
};
std::map<std::string, Setup> g_setups;

class EntityTree : public QTreeWidget
{
protected:
	QMimeData* mimeData( const QList<QTreeWidgetItem*> items ) const override {
		if( items.isEmpty() || items.front()->childCount() != 0 )
			return nullptr;
		auto *data = new QMimeData;
		data->setData( c_mime, items.front()->data( 0, Qt::UserRole ).toString().toLatin1() );
		return data;
	}
	QStringList mimeTypes() const override {
		return { c_mime };
	}
};

void Entity_place( const QString& classname, Vector3 origin, bool onSurface ){
	EntityClass* eclass = GlobalEntityClassManager().findOrInsert( classname.toLatin1().constData(), true );
	if( onSurface && eclass->fixedsize ){
		const Vector3 ray = vector3_normalised( origin - KrCamera_origin() );
		if( std::fabs( ray.z() ) < 0.7f ) // dropped on a wall: in front of it
			origin -= ray * ( std::max( eclass->maxs.x() - eclass->mins.x(), eclass->maxs.y() - eclass->mins.y() ) * 0.5f + 1 );
		else if( ray.z() < 0 )
			origin.z() -= eclass->mins.z(); // on a floor: standing on it, not half in it
		else
			origin.z() -= eclass->maxs.z(); // on a ceiling: hanging under it
	}
	vector3_snap( origin, GetSnapGridSize() );
	GlobalSelectionSystem().setSelectedAll( false );
	Entity_createFromSelection( classname.toLatin1().constData(), origin );
	const Setup& setup = g_setups[classname.toStdString()];
	for( const auto& [key, value] : setup.keys )
		if( !value.empty() )
			Scene_EntitySetKeyValue_Selected( key.c_str(), value.c_str() );
	if( setup.spawnflags != 0 )
		Scene_EntitySetKeyValue_Selected( "spawnflags", StringStream<16>( setup.spawnflags ) );
	globalOutputStream() << "placed " << classname.toLatin1().constData() << " at " << origin << '\n';
}

/// accepts entity drops on the 2D and 3D views
class DropFilter : public QObject
{
protected:
	bool eventFilter( QObject* obj, QEvent* event ) override {
		if( event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ){
			auto *e = static_cast<QDropEvent*>( event );
			if( e->mimeData()->hasFormat( c_mime ) ){
				e->acceptProposedAction();
				return true;
			}
		}
		else if( event->type() == QEvent::Drop ){
			auto *e = static_cast<QDropEvent*>( event );
			if( !e->mimeData()->hasFormat( c_mime ) )
				return false;
			const QString classname = QString::fromLatin1( e->mimeData()->data( c_mime ) );
			auto *widget = static_cast<QWidget*>( obj );
			const QPoint pos = e->pos();
			if( g_pParentWnd->GetCamWnd() != nullptr && widget == CamWnd_getWidget( *g_pParentWnd->GetCamWnd() ) ){
				Vector3 point;
				const bool hit = KrCamera_pointAt( pos.x(), pos.y(), point );
				Entity_place( classname, point, hit );
			}
			else{
				for( XYWnd* xy : { g_pParentWnd->GetXYWnd(), g_pParentWnd->GetXZWnd(), g_pParentWnd->GetYZWnd() } )
					if( xy != nullptr && xy->GetWidget() == widget ){
						const float scale = widget->devicePixelRatioF();
						Vector3 point;
						XYWnd_MouseToPoint( xy, int( pos.x() * scale ), int( pos.y() * scale ), point );
						Entity_place( classname, point, false );
						break;
					}
			}
			e->acceptProposedAction();
			widget->activateWindow();
			return true;
		}
		return QObject::eventFilter( obj, event );
	}
};
DropFilter g_dropFilter;

class EntityBrowser : public QWidget
{
	KeepKeysFilter m_keepKeys;
	QLineEdit* m_search;
	EntityTree* m_tree;
	QLabel* m_title;
	QLabel* m_description;
	QTableWidget* m_keys;
	QGroupBox* m_gametypeBox;
	std::vector<QCheckBox*> m_gametypes;
	QGroupBox* m_flagsBox;
	QGridLayout* m_flagsGrid;
	std::vector<QCheckBox*> m_flags;
	QString m_class;
	bool m_loading = false;

	static bool isSof2(){
		return string_equal_nocase( GlobalRadiant().getGameDescriptionKeyValue( "q3map2_type" ), "sof2" );
	}
	void fill(){
		m_tree->clear();
		struct Collect : public EntityClassVisitor
		{
			std::vector<EntityClass*> classes;
			void visit( EntityClass* e ) override {
				if( e->fixedsize && !string_equal( e->name(), "worldspawn" ) )
					classes.push_back( e );
			}
		} collect;
		GlobalEntityClassManager().forEach( collect );
		std::sort( collect.classes.begin(), collect.classes.end(), []( EntityClass* a, EntityClass* b ){ return string_less_nocase( a->name(), b->name() ); } );
		std::map<QString, QTreeWidgetItem*> groups;
		for( EntityClass* e : collect.classes ){
			const QString name( e->name() );
			const QString group = name.contains( '_' ) ? name.section( '_', 0, 0 ) : QString( "other" );
			QTreeWidgetItem*& g = groups[group];
			if( g == nullptr ){
				g = new QTreeWidgetItem( m_tree, { group } );
				g->setFlags( Qt::ItemIsEnabled );
			}
			auto *item = new QTreeWidgetItem( g, { name } );
			item->setData( 0, Qt::UserRole, name );
			item->setToolTip( 0, QString( e->comments() ).trimmed().left( 300 ) );
			item->setForeground( 0, QColor::fromRgbF( std::clamp( e->color.x(), 0.3f, 1.f ), std::clamp( e->color.y(), 0.3f, 1.f ), std::clamp( e->color.z(), 0.3f, 1.f ) ) );
			item->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled );
		}
		filter();
	}
	void filter(){
		const QString text = m_search->text().trimmed();
		for( int i = 0; i < m_tree->topLevelItemCount(); ++i ){
			QTreeWidgetItem* g = m_tree->topLevelItem( i );
			int shown = 0;
			for( int k = 0; k < g->childCount(); ++k ){
				const bool show = text.isEmpty() || g->child( k )->text( 0 ).contains( text, Qt::CaseInsensitive );
				g->child( k )->setHidden( !show );
				shown += show;
			}
			g->setHidden( shown == 0 );
			g->setExpanded( !text.isEmpty() );
		}
	}
	void select( const QString& classname ){
		m_class = classname;
		m_loading = true;
		EntityClass* e = GlobalEntityClassManager().findOrInsert( classname.toLatin1().constData(), true );
		Setup& setup = g_setups[classname.toStdString()];
		m_title->setText( QString( "<b>%1</b>  - drag it into the 2D or 3D view" ).arg( classname ) );
		m_description->setText( QString( e->comments() ).trimmed() );
		// keys: the definition's keys (+ angle), with what you set
		std::vector<std::pair<QString, QString>> rows; // key, description
		bool angle = false, gametype = false;
		for( const auto& [key, attr] : e->m_attributes ){
			if( string_equal( key.c_str(), "spawnflags" ) )
				continue;
			rows.emplace_back( key.c_str(), QString( attr.m_description.c_str() ).trimmed() );
			angle |= string_equal( key.c_str(), "angle" ) || string_equal( key.c_str(), "angles" );
			gametype |= string_equal( key.c_str(), "gametype" );
		}
		if( !angle )
			rows.emplace( rows.begin(), "angle", "direction it faces, degrees" );
		m_keys->setRowCount( int( rows.size() ) );
		for( int r = 0; r < int( rows.size() ); ++r ){
			auto *k = new QTableWidgetItem( rows[r].first );
			k->setFlags( Qt::ItemIsEnabled );
			k->setToolTip( rows[r].second );
			auto *v = new QTableWidgetItem( QString::fromStdString( setup.keys[rows[r].first.toStdString()] ) );
			v->setToolTip( rows[r].second.isEmpty() ? QString( "empty = not set" ) : rows[r].second );
			m_keys->setItem( r, 0, k );
			m_keys->setItem( r, 1, v );
		}
		// SOF2 gametypes: "gametype" "dm tdm ..." - nothing ticked = every gametype
		m_gametypeBox->setVisible( isSof2() );
		const QStringList types = QString::fromStdString( setup.keys["gametype"] ).split( ' ', Qt::SkipEmptyParts );
		for( QCheckBox* box : m_gametypes )
			box->setChecked( types.contains( box->text() ) );
		// spawnflags
		for( QCheckBox* box : m_flags )
			delete box;
		m_flags.clear();
		for( int i = 0; i < MAX_FLAGS; ++i ){
			if( e->flagnames[i][0] == '\0' || string_equal( e->flagnames[i], "-" ) )
				continue;
			auto *box = new QCheckBox( e->flagnames[i] );
			box->setChecked( setup.spawnflags & ( 1 << i ) );
			box->setProperty( "bit", i );
			QObject::connect( box, &QCheckBox::toggled, [this, i]( bool on ){
				Setup& s = g_setups[m_class.toStdString()];
				s.spawnflags = on ? s.spawnflags | ( 1 << i ) : s.spawnflags & ~( 1 << i );
			} );
			m_flagsGrid->addWidget( box, int( m_flags.size() ) / 3, int( m_flags.size() ) % 3 );
			m_flags.push_back( box );
		}
		m_flagsBox->setVisible( !m_flags.empty() );
		m_loading = false;
		(void)gametype;
	}
	void gametypeChanged(){
		if( m_loading || m_class.isEmpty() )
			return;
		QStringList on;
		for( QCheckBox* box : m_gametypes )
			if( box->isChecked() )
				on << box->text();
		g_setups[m_class.toStdString()].keys["gametype"] = on.join( ' ' ).toStdString();
		// show it in the key list too
		for( int r = 0; r < m_keys->rowCount(); ++r )
			if( m_keys->item( r, 0 )->text() == "gametype" ){
				m_loading = true;
				m_keys->item( r, 1 )->setText( on.join( ' ' ) );
				m_loading = false;
			}
	}

public:
	EntityBrowser( QWidget* parent ) : QWidget( parent ){
		auto *vbox = new QVBoxLayout( this );
		vbox->setContentsMargins( 2, 2, 2, 2 );
		auto *split = new QSplitter( Qt::Horizontal );
		vbox->addWidget( split, 1 );
		{
			auto *left = new QWidget;
			auto *lv = new QVBoxLayout( left );
			lv->setContentsMargins( 0, 0, 0, 0 );
			m_search = new QLineEdit;
			m_search->setPlaceholderText( "Search entities (pickup, info_player, weapon ...)" );
			m_search->setClearButtonEnabled( true );
			m_search->installEventFilter( &m_keepKeys );
			QObject::connect( m_search, &QLineEdit::textChanged, [this](){ filter(); } );
			lv->addWidget( m_search );
			m_tree = new EntityTree;
			m_tree->setHeaderHidden( true );
			m_tree->setDragEnabled( true );
			m_tree->setDragDropMode( QAbstractItemView::DragOnly );
			m_tree->setSelectionMode( QAbstractItemView::SingleSelection );
			QObject::connect( m_tree, &QTreeWidget::currentItemChanged, [this]( QTreeWidgetItem* item, QTreeWidgetItem* ){
				if( item != nullptr && item->childCount() == 0 && item->parent() != nullptr )
					select( item->data( 0, Qt::UserRole ).toString() );
			} );
			QObject::connect( m_tree, &QTreeWidget::itemDoubleClicked, [this]( QTreeWidgetItem* item, int ){
				if( item != nullptr && item->childCount() == 0 && item->parent() != nullptr )
					Entity_place( item->data( 0, Qt::UserRole ).toString(), KrCamera_targetPoint(), true );
			} );
			lv->addWidget( m_tree, 1 );
			split->addWidget( left );
		}
		{
			auto *scroll = new QScrollArea;
			scroll->setWidgetResizable( true );
			auto *right = new QWidget;
			auto *rv = new QVBoxLayout( right );
			m_title = new QLabel( "Pick an entity on the left, set it up here, then drag it into the 2D or 3D view (double-click: middle of the 3D view)." );
			m_title->setWordWrap( true );
			rv->addWidget( m_title );
			m_description = new QLabel;
			m_description->setWordWrap( true );
			m_description->setStyleSheet( "color: palette(mid);" );
			rv->addWidget( m_description );
			m_gametypeBox = new QGroupBox( "Gametypes (none ticked = all)" );
			auto *gh = new QHBoxLayout( m_gametypeBox );
			for( const char* t : { "dm", "tdm", "ctf", "inf", "elim", "dem" } ){
				auto *box = new QCheckBox( t );
				QObject::connect( box, &QCheckBox::toggled, [this]( bool ){ gametypeChanged(); } );
				gh->addWidget( box );
				m_gametypes.push_back( box );
			}
			gh->addStretch();
			m_gametypeBox->setVisible( false );
			rv->addWidget( m_gametypeBox );
			m_flagsBox = new QGroupBox( "Spawnflags" );
			m_flagsGrid = new QGridLayout( m_flagsBox );
			m_flagsBox->setVisible( false );
			rv->addWidget( m_flagsBox );
			m_keys = new QTableWidget( 0, 2 );
			m_keys->setHorizontalHeaderLabels( { "Key", "Value (empty = not set)" } );
			m_keys->horizontalHeader()->setStretchLastSection( true );
			m_keys->verticalHeader()->setVisible( false );
			m_keys->installEventFilter( &m_keepKeys );
			QObject::connect( m_keys, &QTableWidget::itemChanged, [this]( QTableWidgetItem* item ){
				if( m_loading || item->column() != 1 || m_class.isEmpty() )
					return;
				const QString key = m_keys->item( item->row(), 0 )->text();
				g_setups[m_class.toStdString()].keys[key.toStdString()] = item->text().trimmed().toStdString();
				if( key == "gametype" ){
					m_loading = true;
					const QStringList types = item->text().split( ' ', Qt::SkipEmptyParts );
					for( QCheckBox* box : m_gametypes )
						box->setChecked( types.contains( box->text() ) );
					m_loading = false;
				}
			} );
			rv->addWidget( m_keys, 1 );
			scroll->setWidget( right );
			split->addWidget( scroll );
		}
		split->setStretchFactor( 0, 1 );
		split->setStretchFactor( 1, 2 );
		QTimer::singleShot( 0, this, [this](){ fill(); } );
	}
	void showEvent( QShowEvent* event ) override {
		QWidget::showEvent( event );
		if( m_tree->topLevelItemCount() == 0 )
			fill(); // (entity definitions reloaded)
	}
};

} // namespace

QWidget* KrEntityBrowser_constructWindow( QWidget* parent ){
	auto *browser = new EntityBrowser( parent );
	// the views accept the drops once they exist
	QTimer::singleShot( 0, [](){
		if( g_pParentWnd == nullptr )
			return;
		for( XYWnd* xy : { g_pParentWnd->GetXYWnd(), g_pParentWnd->GetXZWnd(), g_pParentWnd->GetYZWnd() } )
			if( xy != nullptr ){
				xy->GetWidget()->setAcceptDrops( true );
				xy->GetWidget()->installEventFilter( &g_dropFilter );
			}
		if( g_pParentWnd->GetCamWnd() != nullptr ){
			QWidget* cam = CamWnd_getWidget( *g_pParentWnd->GetCamWnd() );
			cam->setAcceptDrops( true );
			cam->installEventFilter( &g_dropFilter );
		}
	} );
	return browser;
}
