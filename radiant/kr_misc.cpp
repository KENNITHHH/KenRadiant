/*
   KenRadiant:
   - version / update notice at the right of the menu bar (asks GitHub once at startup, can be turned off)
   - Go To: fly the camera to typed or pasted coordinates

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include <QMenuBar>
#include <QMenu>
#include <QToolButton>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QClipboard>
#include <QGuiApplication>
#include <QRegularExpression>
#include <functional>
#include <QKeyEvent>
#include <QStatusBar>
#include <QResizeEvent>
#include <QCoreApplication>

#include "version.h"
#include "math/vector.h"
#include "commands.h"
#include "preferencesystem.h"
#include "stringio.h"
#include "dialog.h"
#include "gtkutil/widget.h"
#include "gtkutil/menu.h"
#include "mainframe.h"

Vector3 KrCamera_origin();
void KrCamera_goTo( const Vector3& origin );

namespace
{
const char* const c_repo = "KENNITHHH/KenRadiant";

/// the commit this build was made from: RADIANT_VERSION is "<version>-git-<commit>"
QString Build_commit(){
	const QString version( RADIANT_VERSION );
	const int at = version.lastIndexOf( "git-" );
	return at >= 0 ? version.mid( at + 4 ) : QString();
}

bool g_checkUpdates = true;
QToolButton* g_versionButton = nullptr;
QString g_updateUrl = QString( "https://github.com/%1/releases" ).arg( c_repo );

void Version_show( const QString& text, const QString& tip, bool update ){
	if( g_versionButton == nullptr )
		return;
	g_versionButton->setText( text );
	g_versionButton->setToolTip( tip );
	// the menu bar sizes its corner widget only on resize: make room for the new text now
	g_versionButton->setMinimumWidth( g_versionButton->fontMetrics().horizontalAdvance( text ) + 40 );
	g_versionButton->adjustSize();
	if( QWidget* bar = g_versionButton->parentWidget() ){
		QResizeEvent event( bar->size(), bar->size() );
		QCoreApplication::sendEvent( bar, &event );
		bar->update();
	}
	g_versionButton->setStyleSheet( update ? "QToolButton { color: #ffa020; font-weight: bold; font-size: 11pt; padding: 1px 8px; }"
	                                       : "QToolButton { font-size: 10pt; padding: 1px 8px; }" );
}

/// GET with the system's curl (Windows 10+ and every Linux have it): no TLS libraries to ship
void Http_get( const QString& url, std::function<void( const QByteArray& )> done ){
	auto *process = new QProcess;
	QObject::connect( process, QOverload<int, QProcess::ExitStatus>::of( &QProcess::finished ), [process, done]( int code, QProcess::ExitStatus status ){
		const QByteArray data = process->readAllStandardOutput();
		process->deleteLater();
		done( code == 0 && status == QProcess::NormalExit ? data : QByteArray() );
	} );
	QObject::connect( process, &QProcess::errorOccurred, [process, done]( QProcess::ProcessError error ){
		if( error == QProcess::FailedToStart ){
			process->deleteLater();
			done( QByteArray() );
		}
	} );
	process->start( "curl", { "-s", "-L", "-m", "15", "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: KenRadiant", url } );
}

void Update_check( bool verbose ){
	const QString commit = Build_commit();
	Version_show( QString( "KenRadiant %1  (checking...)" ).arg( commit ), "Asking GitHub for the latest build", false );
	Http_get( QString( "https://api.github.com/repos/%1/releases?per_page=1" ).arg( c_repo ), [commit, verbose]( const QByteArray& data ){
		const QJsonArray releases = QJsonDocument::fromJson( data ).array();
		if( releases.isEmpty() ){
			Version_show( QString( "KenRadiant %1" ).arg( commit ), "Couldn't reach GitHub to check for updates", false );
			if( verbose )
				globalWarningStream() << "update check: couldn't reach GitHub (is curl available?)\n";
			return;
		}
		const QJsonObject release = releases[0].toObject();
		const QString tag = release["tag_name"].toString();
		g_updateUrl = release["html_url"].toString( g_updateUrl );
		Http_get( QString( "https://api.github.com/repos/%1/git/ref/tags/%2" ).arg( c_repo, tag ), [commit, tag, verbose]( const QByteArray& data ){
			const QString latest = QJsonDocument::fromJson( data ).object()["object"].toObject()["sha"].toString();
			if( latest.isEmpty() || commit.isEmpty() ){
				Version_show( QString( "KenRadiant %1" ).arg( commit ), "Couldn't read the latest build from GitHub", false );
				return;
			}
			if( latest.startsWith( commit ) ){
				Version_show( QString( "KenRadiant %1" ).arg( commit ), QString( "Up to date (build %1)" ).arg( tag ), false );
				if( verbose )
					globalOutputStream() << "update check: this is the latest build (" << tag.toLatin1().constData() << ")\n";
			}
			else{
				Version_show( QString( "Update available - click to download  (you have %1)" ).arg( commit ),
				              QString( "A newer build is on GitHub (%1, commit %2). Click to open the download page." ).arg( tag, latest.left( 7 ) ), true );
				globalOutputStream() << "update check: a newer KenRadiant build is on GitHub (" << tag.toLatin1().constData() << ", " << latest.left( 7 ).toLatin1().constData() << ")\n";
			}
		} );
	} );
}

ToggleItem g_checkUpdates_item{ BoolExportCaller( g_checkUpdates ) };
void CheckUpdates_toggle(){
	g_checkUpdates = !g_checkUpdates;
	g_checkUpdates_item.update();
}
void CheckUpdates_now(){
	Update_check( true );
}

/* ---- Go To coordinates ---- */

/// the editor's shortcuts are application wide (Enter = clip ...): text boxes keep their keys
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

class GoToDialog : public QDialog
{
	QDoubleSpinBox* m_xyz[3];
	QLineEdit* m_text;
	KeepKeysFilter m_keepKeys;
public:
	GoToDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ){
		setWindowTitle( "Go To" );
		auto *vbox = new QVBoxLayout( this );
		m_text = new QLineEdit;
		m_text->setPlaceholderText( "x y z  (paste a position, an origin key, a console line ...)" );
		QObject::connect( m_text, &QLineEdit::textEdited, [this]( const QString& t ){ parse( t ); } );
		QObject::connect( m_text, &QLineEdit::returnPressed, [this](){ go(); } );
		m_text->installEventFilter( &m_keepKeys );
		vbox->addWidget( m_text );
		auto *grid = new QGridLayout;
		const char* names[3] = { "X", "Y", "Z" };
		for( int i = 0; i < 3; ++i ){
			grid->addWidget( new QLabel( names[i] ), 0, i * 2 );
			m_xyz[i] = new QDoubleSpinBox;
			m_xyz[i]->setRange( -131072, 131072 );
			m_xyz[i]->setDecimals( 1 );
			m_xyz[i]->setSingleStep( 16 );
			m_xyz[i]->installEventFilter( &m_keepKeys );
			grid->addWidget( m_xyz[i], 0, i * 2 + 1 );
		}
		vbox->addLayout( grid );
		auto *buttons = new QHBoxLayout;
		auto *paste = new QPushButton( "Paste" );
		paste->setToolTip( "Take the coordinates from the clipboard" );
		QObject::connect( paste, &QPushButton::clicked, [this](){ m_text->setText( QGuiApplication::clipboard()->text() ); parse( m_text->text() ); } );
		auto *go = new QPushButton( "Go" );
		go->setDefault( true );
		QObject::connect( go, &QPushButton::clicked, [this](){ this->go(); } );
		auto *close = new QPushButton( "Close" );
		QObject::connect( close, &QPushButton::clicked, [this](){ hide(); } );
		auto *current = new QPushButton( "Current" );
		current->setToolTip( "Fill in where the 3D camera is now" );
		QObject::connect( current, &QPushButton::clicked, [this](){ fill(); } );
		auto *copy = new QPushButton( "Copy" );
		copy->setToolTip( "Copy these coordinates to the clipboard" );
		QObject::connect( copy, &QPushButton::clicked, [this](){
			QGuiApplication::clipboard()->setText( QString( "%1 %2 %3" ).arg( m_xyz[0]->value() ).arg( m_xyz[1]->value() ).arg( m_xyz[2]->value() ) );
		} );
		buttons->addWidget( paste );
		buttons->addWidget( current );
		buttons->addWidget( copy );
		buttons->addStretch();
		buttons->addWidget( go );
		buttons->addWidget( close );
		vbox->addLayout( buttons );
	}
	void parse( const QString& text ){
		QRegularExpression number( "-?\\d+(\\.\\d+)?" );
		auto it = number.globalMatch( text );
		for( int i = 0; i < 3 && it.hasNext(); ++i )
			m_xyz[i]->setValue( it.next().captured( 0 ).toDouble() );
	}
	void fill(){
		const Vector3 o = KrCamera_origin();
		for( int i = 0; i < 3; ++i )
			m_xyz[i]->setValue( o[i] );
		m_text->clear();
	}
	void go(){
		const Vector3 target( float( m_xyz[0]->value() ), float( m_xyz[1]->value() ), float( m_xyz[2]->value() ) );
		KrCamera_goTo( target );
		globalOutputStream() << "go to " << target << '\n';
	}
};
GoToDialog* g_goTo = nullptr;
void GoTo_show(){
	if( g_goTo == nullptr )
		g_goTo = new GoToDialog( MainFrame_getWindow() );
	g_goTo->fill();
	g_goTo->show();
	g_goTo->raise();
	g_goTo->activateWindow();
}

} // namespace

void KrMisc_registerCommands(){
	GlobalToggles_insert( "ToggleCheckUpdates", makeCallbackF( CheckUpdates_toggle ), ToggleItem::AddCallbackCaller( g_checkUpdates_item ) );
	GlobalCommands_insert( "CheckForKenRadiantUpdate", makeCallbackF( CheckUpdates_now ) );
	GlobalCommands_insert( "GoToCoordinates", makeCallbackF( GoTo_show ), QKeySequence( "Ctrl+Shift+G" ) );
	GlobalPreferenceSystem().registerPreference( "KrCheckUpdates", BoolImportStringCaller( g_checkUpdates ), BoolExportStringCaller( g_checkUpdates ) );
}

/// the version / update notice at the right of the menu bar
void KrMisc_installMenuCorner( QMenuBar* menubar ){
	g_versionButton = new QToolButton;
	g_versionButton->setAutoRaise( true );
	Version_show( QString( "KenRadiant %1" ).arg( Build_commit() ), "This build", false );
	QObject::connect( g_versionButton, &QToolButton::clicked, [](){ QDesktopServices::openUrl( QUrl( g_updateUrl ) ); } );
	menubar->setCornerWidget( g_versionButton, Qt::TopRightCorner );
	if( g_checkUpdates )
		QTimer::singleShot( 3000, [](){ Update_check( false ); } );
}

/// the 3D camera position, always at the bottom of the window (click: copy it)
void KrMisc_installStatusCamera( QStatusBar* statusbar ){
	auto *button = new QToolButton;
	button->setAutoRaise( true );
	button->setToolTip( "3D camera position - click to copy it. Go To (Ctrl+Shift+G) flies to coordinates." );
	statusbar->addPermanentWidget( button, 0 );
	QObject::connect( button, &QToolButton::clicked, [](){
		const Vector3 o = KrCamera_origin();
		QGuiApplication::clipboard()->setText( QString( "%1 %2 %3" ).arg( std::round( o.x() ) ).arg( std::round( o.y() ) ).arg( std::round( o.z() ) ) );
		globalOutputStream() << "copied camera position " << o << '\n';
	} );
	auto *timer = new QTimer( button );
	QObject::connect( timer, &QTimer::timeout, [button](){
		const Vector3 o = KrCamera_origin();
		const QString text = QString( "cam %1 %2 %3" ).arg( int( std::round( o.x() ) ) ).arg( int( std::round( o.y() ) ) ).arg( int( std::round( o.z() ) ) );
		if( button->text() != text )
			button->setText( text );
	} );
	timer->start( 150 );
}
