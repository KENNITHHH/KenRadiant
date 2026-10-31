/*
   KenRadiant: Map Text window - the .map as text, kept in sync with the selection both ways:
   select something in a view and the text jumps to its entity / brush and highlights it,
   click in the text and that entity / brush is selected in the views (double-click flies the camera there).

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <vector>
#include <unordered_map>

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QListWidget>
#include <algorithm>
#include <QTimer>
#include <QFontDatabase>
#include <QScrollBar>
#include <QGuiApplication>
#include <functional>
#include <utility>
#include <QKeyEvent>

#include "gtkutil/guisettings.h"
#include "imap.h"
#include "ientity.h"
#include "typesystem.h"
#include "ientity.h"
#include "iselection.h"
#include "iscenegraph.h"
#include "scenelib.h"
#include "stream/stringstream.h"
#include "map.h"
#include "mainframe.h"
#include "commands.h"
#include "gtkutil/widget.h"
#include "dialog.h"
#include "signal/isignal.h"

void GlobalCamera_FocusOnSelected();

namespace
{

inline MapExporter* Node_getMapExporter( scene::Node& node ){ // like the map writer
	return NodeTypeCast<MapExporter>::cast( node );
}

struct TextBlock
{
	int start = 0, end = 0; // character range in the text
	scene::Node* entity = nullptr;
	scene::Node* brush = nullptr; // nullptr: the entity's own lines (keys)
	std::size_t entityIndex = 0;
};

/// the scene in the order the map writer walks it (entities, then their brushes / patches)
struct SceneOrder
{
	struct Item{ scene::Node* entity; std::vector<scene::Node*> children; };
	std::vector<Item> items;
};

SceneOrder Scene_order(){
	SceneOrder order;
	if( scene::Traversable* root = Node_getTraversable( GlobalSceneGraph().root() ) ){
		class EntityWalker : public scene::Traversable::Walker
		{
			SceneOrder& m_order;
		public:
			EntityWalker( SceneOrder& order ) : m_order( order ){}
			bool pre( scene::Node& node ) const override {
				Entity* entity = Node_getEntity( node );
				if( entity == nullptr )
					return false;
				scene::Traversable* traversable = Node_getTraversable( node );
				// the writer drops empty group entities
				if( entity->isContainer() && ( traversable == nullptr || traversable->empty() ) && !string_equal( entity->getClassName(), "worldspawn" ) && !entity->hasKeyValue( "origin" ) )
					return false;
				SceneOrder::Item item{ &node, {} };
				if( traversable != nullptr ){
					class ChildWalker : public scene::Traversable::Walker
					{
						std::vector<scene::Node*>& m_children;
					public:
						ChildWalker( std::vector<scene::Node*>& children ) : m_children( children ){}
						bool pre( scene::Node& child ) const override {
							if( Node_getMapExporter( child ) != nullptr )
								m_children.push_back( &child );
							return false;
						}
					};
					traversable->traverse( ChildWalker( item.children ) );
				}
				m_order.items.push_back( std::move( item ) );
				return false;
			}
		};
		root->traverse( EntityWalker( order ) );
	}
	return order;
}

/// the editor's shortcuts are application wide (Enter = clip, letters ...): text boxes keep their keys
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

class MapTextDialog : public QDialog
{
	KeepKeysFilter m_keepKeys;
	QPlainTextEdit* m_text;
	QLabel* m_status;
	QLineEdit* m_find;
	QListWidget* m_list;
	QPushButton* m_selectAll;
	std::vector<int> m_results; // blocks containing the find text
	std::size_t m_entityCount = 0;
	QTimer m_searchTimer;
	QCheckBox* m_follow;
	QTimer m_refreshTimer, m_selectionTimer;
	std::vector<TextBlock> m_blocks;
	std::unordered_map<const scene::Node*, std::size_t> m_blockOf; // node -> block index
	bool m_dirty = true;
	bool m_pendingScroll = false; // a selection came in while the text was out of date: jump when rebuilt
	QString m_lastText;
	bool m_selecting = false; // we are selecting from the text: don't jump back
	int m_highlighted = -1;

	void rebuild(){
		m_dirty = false;
		StringOutputStream out( 1 << 20 );
		Map_getFormat( g_map ).writeGraph( GlobalSceneGraph().root(), Map_Traverse, out );
		const QString text = QString::fromLatin1( out.c_str() );
		const bool same = text == m_lastText; // selecting things changes nothing in the text: keep it, just re-highlight
		m_lastText = text;

		// blocks from the writer's "// entity N" and "// brush N" comments, in scene order
		const SceneOrder order = Scene_order();
		m_blocks.clear();
		m_blockOf.clear();
		std::size_t entity = std::size_t( -1 ), child = 0;
		int pos = 0;
		std::vector<int> entityStarts;
		while( pos < text.size() ){
			int end = text.indexOf( '\n', pos );
			if( end < 0 )
				end = text.size();
			const QStringRef line = text.midRef( pos, end - pos ).trimmed();
			if( line.startsWith( QLatin1String( "// entity " ) ) ){
				++entity;
				child = 0;
				if( entity < order.items.size() ){
					m_blocks.push_back( { pos, end, order.items[entity].entity, nullptr, entity } );
					entityStarts.push_back( int( m_blocks.size() ) - 1 );
				}
			}
			else if( line.startsWith( QLatin1String( "// brush " ) ) && entity < order.items.size() ){
				if( child < order.items[entity].children.size() )
					m_blocks.push_back( { pos, end, order.items[entity].entity, order.items[entity].children[child], entity } );
				++child;
			}
			pos = end + 1;
		}
		// block ends: a brush ends where the next marker starts; an entity spans all its brushes
		for( std::size_t i = 0; i < m_blocks.size(); ++i )
			m_blocks[i].end = i + 1 < m_blocks.size() ? m_blocks[i + 1].start : text.size();
		for( std::size_t k = 0; k < entityStarts.size(); ++k ){
			TextBlock& b = m_blocks[entityStarts[k]];
			b.end = k + 1 < entityStarts.size() ? m_blocks[entityStarts[k + 1]].start : text.size();
		}
		for( std::size_t i = 0; i < m_blocks.size(); ++i )
			m_blockOf[m_blocks[i].brush != nullptr ? m_blocks[i].brush : m_blocks[i].entity] = i;

		if( same ){
			showSelection( std::exchange( m_pendingScroll, false ) );
			return;
		}
		// keep the view where it was
		const int scroll = m_text->verticalScrollBar()->value();
		const int cursor = m_text->textCursor().position();
		{
			const QSignalBlocker b( m_text );
			m_text->setPlainText( text );
			QTextCursor c = m_text->textCursor();
			c.setPosition( std::min( cursor, int( text.size() ) ) );
			m_text->setTextCursor( c );
		}
		m_text->verticalScrollBar()->setValue( scroll );
		m_highlighted = -1;
		m_entityCount = order.items.size();
		m_status->setText( QString( "%1 entities, %2 lines" ).arg( order.items.size() ).arg( m_text->blockCount() ) );
		if( !m_find->text().trimmed().isEmpty() ) // the hits moved with the edit
			m_searchTimer.start();
		showSelection( std::exchange( m_pendingScroll, false ) );
	}

	/// blocks are in text order and an entity's marker comes before its brushes: the last block starting at or before
	/// the position is the innermost one
	int blockAt( int position ) const {
		auto it = std::upper_bound( m_blocks.begin(), m_blocks.end(), position, []( int p, const TextBlock& b ){ return p < b.start; } );
		if( it == m_blocks.begin() )
			return -1;
		--it;
		return position < it->end || it->brush == nullptr ? int( it - m_blocks.begin() ) : -1;
	}
	/// every block that contains the text (a texture, a model, a key ...)
	std::vector<int> blocksWith( const QString& what ) const {
		std::vector<int> found;
		const QString& text = m_lastText;
		for( int pos = text.indexOf( what, 0, Qt::CaseInsensitive ); pos >= 0; pos = text.indexOf( what, pos + what.size(), Qt::CaseInsensitive ) ){
			const int b = blockAt( pos );
			if( b >= 0 && ( found.empty() || found.back() != b ) )
				found.push_back( b );
		}
		return found;
	}
	QString describe( int index ) const {
		const TextBlock& b = m_blocks[index];
		const Entity* entity = Node_getEntity( *b.entity );
		const char* classname = entity != nullptr ? entity->getClassName() : "?";
		if( b.brush != nullptr ){
			const int line = m_text->document()->findBlock( b.start ).blockNumber() + 1;
			return QString( "entity %1 (%2), %3 - line %4" ).arg( b.entityIndex ).arg( classname ).arg( m_lastText.mid( b.start, 40 ).section( '\n', 0, 0 ).trimmed().mid( 3 ) ).arg( line );
		}
		QString extra;
		if( entity != nullptr && !string_empty( entity->getKeyValue( "model" ) ) )
			extra = QString( " %1" ).arg( entity->getKeyValue( "model" ) );
		else if( entity != nullptr && !string_empty( entity->getKeyValue( "targetname" ) ) )
			extra = QString( " \"%1\"" ).arg( entity->getKeyValue( "targetname" ) );
		return QString( "entity %1 (%2)%3" ).arg( b.entityIndex ).arg( classname ).arg( extra );
	}
	scene::Instance* instanceOf( int index ) const {
		const TextBlock& b = m_blocks[index];
		scene::Path path( makeReference( GlobalSceneGraph().root() ) );
		path.push( makeReference( *b.entity ) );
		if( b.brush != nullptr )
			path.push( makeReference( *b.brush ) );
		return GlobalSceneGraph().find( path );
	}
	void highlight( int index, bool scroll ){
		m_highlighted = index;
		QList<QTextEdit::ExtraSelection> extras;
		if( index >= 0 ){
			QTextEdit::ExtraSelection sel;
			sel.format.setBackground( QColor( 255, 140, 13, 70 ) );
			sel.format.setProperty( QTextFormat::FullWidthSelection, true );
			sel.cursor = QTextCursor( m_text->document() );
			sel.cursor.setPosition( m_blocks[index].start );
			sel.cursor.setPosition( std::max( m_blocks[index].start, m_blocks[index].end - 1 ), QTextCursor::KeepAnchor );
			extras.push_back( sel );
			if( scroll ){
				const QSignalBlocker b( m_text );
				QTextCursor c( m_text->document() );
				c.setPosition( m_blocks[index].start );
				m_text->setTextCursor( c );
				m_text->centerCursor();
			}
		}
		m_text->setExtraSelections( extras );
	}
	/// the view selection -> the text
	void showSelection( bool scroll ){
		if( GlobalSelectionSystem().countSelected() == 0 ){
			highlight( -1, false );
			return;
		}
		const scene::Path& path = GlobalSelectionSystem().ultimateSelected().path();
		auto it = m_blockOf.find( &path.top().get() );
		if( it == m_blockOf.end() && path.size() >= 2 )
			it = m_blockOf.find( &path[1].get() );
		highlight( it != m_blockOf.end() ? int( it->second ) : -1, scroll );
	}
	/// the text -> the view selection
	void selectFromText( bool focus ){
		const int index = blockAt( m_text->textCursor().position() );
		if( index < 0 )
			return;
		scene::Instance* instance = instanceOf( index );
		if( instance == nullptr )
			return;
		m_selecting = true;
		if( GlobalSelectionSystem().Mode() != SelectionSystem::ePrimitive )
			GlobalSelectionSystem().SetMode( SelectionSystem::ePrimitive );
		GlobalSelectionSystem().setSelectedAll( false );
		Instance_setSelected( *instance, true );
		m_selecting = false;
		highlight( index, false );
		if( focus )
			GlobalCamera_FocusOnSelected();
	}
	/// Enter / Next / Previous: the next hit in the text, selected in the views
	void find( bool backwards ){
		const QString what = m_find->text();
		if( what.isEmpty() )
			return;
		if( m_dirty )
			rebuild();
		QTextDocument::FindFlags flags;
		if( backwards )
			flags |= QTextDocument::FindBackward;
		if( !m_text->find( what, flags ) ){ // wrap around
			QTextCursor c = m_text->textCursor();
			c.movePosition( backwards ? QTextCursor::End : QTextCursor::Start );
			m_text->setTextCursor( c );
			if( !m_text->find( what, flags ) ){
				m_status->setText( QString( "\"%1\" not found" ).arg( what ) );
				return;
			}
		}
		m_text->centerCursor();
		m_pendingScroll = false;
		selectFromText( false );
		const int index = blockAt( m_text->textCursor().position() );
		const auto it = std::find( m_results.begin(), m_results.end(), index );
		m_status->setText( QString( "\"%1\": %2 of %3" ).arg( what ).arg( it != m_results.end() ? int( it - m_results.begin() ) + 1 : 0 ).arg( m_results.size() ) );
		if( it != m_results.end() ){
			const QSignalBlocker b( m_list );
			m_list->setCurrentRow( int( it - m_results.begin() ) );
		}
	}
	/// typing in the find box: list every brush / entity that contains the text
	void search(){
		const QString what = m_find->text().trimmed();
		m_list->clear();
		m_results.clear();
		if( what.size() < 2 ){
			m_list->setVisible( false );
			m_selectAll->setEnabled( false );
			m_status->setText( QString( "%1 entities, %2 lines" ).arg( m_entityCount ).arg( m_text->blockCount() ) );
			return;
		}
		if( m_dirty )
			rebuild();
		m_results = blocksWith( what );
		constexpr std::size_t shown = 2000;
		for( std::size_t i = 0; i < m_results.size() && i < shown; ++i )
			m_list->addItem( describe( m_results[i] ) );
		if( m_results.size() > shown )
			m_list->addItem( QString( "... %1 more" ).arg( m_results.size() - shown ) );
		m_list->setVisible( !m_results.empty() );
		m_list->setFixedHeight( std::min( 160, m_list->sizeHintForRow( 0 ) * std::max( 1, m_list->count() ) + 2 * m_list->frameWidth() + 2 ) );
		m_selectAll->setEnabled( !m_results.empty() );
		m_status->setText( m_results.empty() ? QString( "\"%1\" not found" ).arg( what )
		                                     : QString( "\"%1\" in %2 brushes / entities - Enter: next, Select all: all of them" ).arg( what ).arg( m_results.size() ) );
	}
	void goToResult( int row, bool focus ){
		if( row < 0 || row >= int( m_results.size() ) )
			return;
		const TextBlock& b = m_blocks[m_results[row]];
		// put the cursor on the hit inside the block
		const int hit = m_lastText.indexOf( m_find->text().trimmed(), b.start, Qt::CaseInsensitive );
		QTextCursor c( m_text->document() );
		c.setPosition( hit >= 0 && hit < b.end ? hit : b.start );
		{
			const QSignalBlocker blocker( m_text );
			m_text->setTextCursor( c );
		}
		m_text->centerCursor();
		selectFromText( focus );
	}
	void selectAllResults(){
		if( m_results.empty() )
			return;
		m_selecting = true;
		if( GlobalSelectionSystem().Mode() != SelectionSystem::ePrimitive )
			GlobalSelectionSystem().SetMode( SelectionSystem::ePrimitive );
		GlobalSelectionSystem().setSelectedAll( false );
		std::size_t count = 0;
		for( int index : m_results )
			if( scene::Instance* instance = instanceOf( index ) ){
				Instance_setSelected( *instance, true );
				++count;
			}
		m_selecting = false;
		m_status->setText( QString( "selected %1 brushes / entities with \"%2\"" ).arg( count ).arg( m_find->text().trimmed() ) );
	}

	class Editor : public QPlainTextEdit
	{
	public:
		std::function<void()> doubleClicked;
	protected:
		void mouseDoubleClickEvent( QMouseEvent* event ) override {
			QPlainTextEdit::mouseDoubleClickEvent( event );
			if( doubleClicked ) doubleClicked();
		}
	};

public:
	MapTextDialog( QWidget* parent ) : QDialog( parent, Qt::Window ){
		setWindowTitle( "Map Text" );
		resize( 720, 800 );
		auto *vbox = new QVBoxLayout( this );
		{
			auto *row = new QHBoxLayout;
			m_find = new QLineEdit;
			m_find->setPlaceholderText( "Find a texture, model, key ... (Enter = next, Shift+Enter = previous)" );
			m_find->setClearButtonEnabled( true );
			m_find->installEventFilter( &m_keepKeys );
			QObject::connect( m_find, &QLineEdit::returnPressed, [this](){ find( QGuiApplication::keyboardModifiers() & Qt::ShiftModifier ); } );
			QObject::connect( m_find, &QLineEdit::textEdited, [this](){ m_searchTimer.start(); } );
			row->addWidget( m_find, 1 );
			auto *prev = new QPushButton( "<" );
			prev->setToolTip( "Previous hit (Shift+Enter)" );
			prev->setAutoDefault( false );
			prev->setFixedWidth( 28 );
			QObject::connect( prev, &QPushButton::clicked, [this](){ find( true ); } );
			auto *next = new QPushButton( ">" );
			next->setToolTip( "Next hit (Enter)" );
			next->setAutoDefault( false );
			next->setFixedWidth( 28 );
			QObject::connect( next, &QPushButton::clicked, [this](){ find( false ); } );
			m_selectAll = new QPushButton( "Select all" );
			m_selectAll->setToolTip( "Select every brush / entity that contains the text (e.g. all brushes with a texture, all models of a kind)" );
			m_selectAll->setAutoDefault( false );
			m_selectAll->setEnabled( false );
			QObject::connect( m_selectAll, &QPushButton::clicked, [this](){ selectAllResults(); } );
			row->addWidget( prev );
			row->addWidget( next );
			row->addWidget( m_selectAll );
			m_follow = new QCheckBox( "Follow selection" );
			m_follow->setChecked( true );
			m_follow->setToolTip( "Jump to the entity / brush you select in the views" );
			row->addWidget( m_follow );
			auto *refresh = new QPushButton( "Refresh" );
			refresh->setAutoDefault( false ); // Enter in the find box searches, it doesn't press Refresh
			QObject::connect( refresh, &QPushButton::clicked, [this](){ rebuild(); } );
			row->addWidget( refresh );
			vbox->addLayout( row );
		}
		auto *editor = new Editor;
		m_text = editor;
		m_text->setReadOnly( true );
		m_text->setLineWrapMode( QPlainTextEdit::NoWrap );
		m_text->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
		m_text->setTextInteractionFlags( Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard );
		m_list = new QListWidget;
		m_list->setVisible( false );
		m_list->setMaximumHeight( 160 );
		m_list->setToolTip( "Click: select it and show its lines; double-click: fly there" );
		QObject::connect( m_list, &QListWidget::currentRowChanged, [this]( int row ){ goToResult( row, false ); } );
		QObject::connect( m_list, &QListWidget::itemDoubleClicked, [this]( QListWidgetItem* item ){ goToResult( m_list->row( item ), true ); } );
		vbox->addWidget( m_list );
		m_searchTimer.setSingleShot( true );
		m_searchTimer.setInterval( 250 );
		QObject::connect( &m_searchTimer, &QTimer::timeout, [this](){ search(); } );
		vbox->addWidget( m_text, 1 );
		m_status = new QLabel;
		vbox->addWidget( m_status );
		auto *help = new QLabel( "Click a line to select its brush / entity in the views, double-click to fly there. "
		                         "Read only - edit the map in the views; the text follows your changes." );
		help->setWordWrap( true );
		vbox->addWidget( help );

		QObject::connect( m_text, &QPlainTextEdit::cursorPositionChanged, [this](){
			if( m_text->hasFocus() && QGuiApplication::mouseButtons() != Qt::NoButton )
				selectFromText( false );
		} );
		editor->doubleClicked = [this](){ selectFromText( true ); };

		m_refreshTimer.setSingleShot( true );
		m_refreshTimer.setInterval( 600 );
		QObject::connect( &m_refreshTimer, &QTimer::timeout, [this](){ if( isVisible() ) rebuild(); else m_dirty = true; } );
		m_selectionTimer.setSingleShot( true );
		m_selectionTimer.setInterval( 0 );
		QObject::connect( &m_selectionTimer, &QTimer::timeout, [this](){
			if( !isVisible() || m_selecting || !m_follow->isChecked() )
				return;
			if( m_dirty ){ // (selecting also notifies a scene change) bring the text up to date first
				m_pendingScroll = true;
				rebuild();
			}
			else
				showSelection( true );
		} );
	}
	void sceneChanged(){
		m_dirty = true;
		if( isVisible() )
			m_refreshTimer.start();
	}
	void selectionChanged(){
		if( !m_selecting )
			m_selectionTimer.start();
	}
	void keyPressEvent( QKeyEvent* event ) override {
		if( event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter )
			return; // never "accept" the dialog
		QDialog::keyPressEvent( event );
	}
	void showEvent( QShowEvent* event ) override {
		QDialog::showEvent( event );
		if( m_dirty )
			rebuild();
		showSelection( true );
	}
};

MapTextDialog* g_mapText = nullptr;

void MapText_sceneChanged(){
	if( g_mapText != nullptr )
		g_mapText->sceneChanged();
}
void MapText_selectionChanged( const Selectable& ){
	if( g_mapText != nullptr )
		g_mapText->selectionChanged();
}

void MapText_show(){
	if( g_mapText == nullptr ){
		g_mapText = new MapTextDialog( MainFrame_getWindow() ); g_guiSettings.addWindow( g_mapText, "KenRadiant/MapText" ); // where you left it, next time too
		QObject::connect( g_mapText, &QObject::destroyed, [](){ g_mapText = nullptr; } ); // closing the editor: no callbacks into a deleted window
		AddSceneChangeCallback( FreeCaller<void(), MapText_sceneChanged>() );
		typedef FreeCaller<void(const Selectable&), MapText_selectionChanged> SelectionChangedCaller;
		GlobalSelectionSystem().addSelectionChangeCallback( SelectionChangedCaller() );
	}
	g_mapText->show();
	g_mapText->raise();
	g_mapText->activateWindow();
}

} // namespace

/// select the map's Nth entity (and its Nth brush / patch; -1 = the entity itself), numbered like the .map's
/// "// entity N" / "// brush N" comments and q3map2's messages
bool KrMapText_selectPrimitive( int entity, int brush, bool focus ){
	const SceneOrder order = Scene_order();
	if( entity < 0 || entity >= int( order.items.size() ) )
		return false;
	const SceneOrder::Item& item = order.items[entity];
	scene::Path path( makeReference( GlobalSceneGraph().root() ) );
	path.push( makeReference( *item.entity ) );
	if( brush >= 0 && brush < int( item.children.size() ) )
		path.push( makeReference( *item.children[brush] ) );
	else if( brush >= 0 || Node_getEntity( *item.entity )->isContainer() ){
		if( item.children.empty() )
			return false;
		if( brush < 0 ) // a brush entity: all its brushes
			path.push( makeReference( *item.children[0] ) );
		else
			return false;
	}
	scene::Instance* instance = GlobalSceneGraph().find( path );
	if( instance == nullptr )
		return false;
	if( GlobalSelectionSystem().Mode() != SelectionSystem::ePrimitive )
		GlobalSelectionSystem().SetMode( SelectionSystem::ePrimitive );
	GlobalSelectionSystem().setSelectedAll( false );
	Instance_setSelected( *instance, true );
	if( focus )
		GlobalCamera_FocusOnSelected();
	return true;
}

void KrMapText_registerCommands(){
	GlobalCommands_insert( "MapTextWindow", makeCallbackF( MapText_show ) );
}
