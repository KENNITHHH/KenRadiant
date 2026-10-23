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
#include <QTimer>
#include <QFontDatabase>
#include <QScrollBar>
#include <QGuiApplication>
#include <functional>
#include <utility>
#include <QKeyEvent>

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

class MapTextDialog : public QDialog
{
	QPlainTextEdit* m_text;
	QLabel* m_status;
	QLineEdit* m_find;
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
		m_status->setText( QString( "%1 entities, %2 lines" ).arg( order.items.size() ).arg( m_text->blockCount() ) );
		showSelection( std::exchange( m_pendingScroll, false ) );
	}

	int blockAt( int position ) const {
		int best = -1;
		for( std::size_t i = 0; i < m_blocks.size(); ++i )
			if( position >= m_blocks[i].start && position < m_blocks[i].end )
				if( best < 0 || m_blocks[i].brush != nullptr ) // the brush inside the entity wins
					best = int( i );
		return best;
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
		const TextBlock& b = m_blocks[index];
		scene::Path path( makeReference( GlobalSceneGraph().root() ) );
		path.push( makeReference( *b.entity ) );
		if( b.brush != nullptr )
			path.push( makeReference( *b.brush ) );
		scene::Instance* instance = GlobalSceneGraph().find( path );
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
		// select what was found in the views too
		m_pendingScroll = false;
		selectFromText( false );
		const int index = blockAt( m_text->textCursor().position() );
		m_status->setText( index >= 0 ? QString( "found \"%1\" - selected in the views" ).arg( what ) : QString( "found \"%1\"" ).arg( what ) );
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
			m_find->setPlaceholderText( "Find (Enter = next, Shift+Enter = previous)" );
			QObject::connect( m_find, &QLineEdit::returnPressed, [this](){ find( QGuiApplication::keyboardModifiers() & Qt::ShiftModifier ); } );
			row->addWidget( m_find, 1 );
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
		g_mapText = new MapTextDialog( MainFrame_getWindow() );
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

void KrMapText_registerCommands(){
	GlobalCommands_insert( "MapTextWindow", makeCallbackF( MapText_show ) );
}
