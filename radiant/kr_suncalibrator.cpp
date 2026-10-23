/*
   KenRadiant: Sun Calibrator - tweak the sky shader's q3map_sun / q3map_sunExt / q3map_skyLight with live preview,
   write the result back into the .shader file, and preview q3map2 compile settings (-gamma, -compensate,
   -saturation, -bounce approximation) on the live light preview.

   This file is part of KenRadiant (GPL, see LICENSE).
 */

#include "kr_lightpreview.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QColorDialog>
#include <QClipboard>
#include <QGuiApplication>
#include <QFile>
#include <QMessageBox>
#include <QRegularExpression>

#include "ishaders.h"
#include "ifilesystem.h"
#include "stream/stringstream.h"
#include "mainframe.h"
#include "qe3.h"

void KrLightPreview_redraw();
void RefreshShaders();
void Build_setPreset( const char* name, const std::vector<CopiedString>& commands );

namespace
{

/// slider + spin box editing one float
class KrValue
{
public:
	QSlider* slider;
	QDoubleSpinBox* spin;
	float scale;
	KrValue( float lo, float hi, float step, int decimals ) : scale( 1.f / step ){
		slider = new QSlider( Qt::Horizontal );
		slider->setRange( int( lo * scale ), int( hi * scale ) );
		slider->setMinimumWidth( 180 );
		spin = new QDoubleSpinBox;
		spin->setRange( lo, hi );
		spin->setSingleStep( step );
		spin->setDecimals( decimals );
		spin->setKeyboardTracking( false );
		QObject::connect( slider, &QSlider::valueChanged, [this]( int v ){
			const QSignalBlocker b( spin );
			spin->setValue( v / scale );
			if( m_changed ) m_changed();
		} );
		QObject::connect( spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [this]( double v ){
			const QSignalBlocker b( slider );
			slider->setValue( int( std::lround( v * scale ) ) );
			if( m_changed ) m_changed();
		} );
	}
	float value() const { return float( spin->value() ); }
	void set( float v ){
		const QSignalBlocker b1( slider ), b2( spin );
		spin->setValue( v );
		slider->setValue( int( std::lround( v * scale ) ) );
	}
	std::function<void()> m_changed;
};

/// keeps the original text of values that did not change (no rounding of 0.8751 to 0.88) and any extra values
/// the line had (q3map_skyLight's optional horizon / sample settings)
QString KrMergeLine( const QString& original, const QString& line, const std::vector<float>& tolerance ){
	const QStringList old = original.split( QRegularExpression( "\\s+" ), Qt::SkipEmptyParts );
	QStringList out = line.split( ' ', Qt::SkipEmptyParts );
	const bool sameKeyword = !old.isEmpty() && old[0].compare( out[0], Qt::CaseInsensitive ) == 0;
	for( int i = 1; i < out.size() && i < old.size(); ++i ){
		bool ok = false;
		const float was = old[i].toFloat( &ok );
		const float tol = i - 1 < int( tolerance.size() ) ? tolerance[i - 1] : 0.f;
		if( ok && std::fabs( was - out[i].toFloat() ) <= tol )
			out[i] = old[i];
	}
	if( sameKeyword )
		for( int i = out.size(); i < old.size(); ++i ){
			if( old[i].startsWith( "//" ) ) // keep a trailing comment
				{ out << old.mid( i ).join( ' ' ); break; }
			out << old[i];
		}
	return out.join( ' ' );
}
const std::vector<float> c_sunTolerance{ 0.005f, 0.005f, 0.005f, 2.5f, 0.5f, 0.5f };
const std::vector<float> c_sunExtTolerance{ 0.005f, 0.005f, 0.005f, 2.5f, 0.5f, 0.5f, 0.25f, 0.5f };
const std::vector<float> c_skyTolerance{ 2.5f, 0.5f };

class SunCalibrator : public QDialog
{
	QLabel* m_shaderLabel;
	QPushButton* m_colourButton;
	KrValue m_r{ 0, 1, 0.01f, 2 }, m_g{ 0, 1, 0.01f, 2 }, m_b{ 0, 1, 0.01f, 2 };
	KrValue m_intensity{ 0, 3000, 5, 0 };
	KrValue m_degrees{ 0, 360, 1, 0 };
	KrValue m_elevation{ -90, 90, 1, 0 };
	QCheckBox* m_ext;
	KrValue m_deviance{ 0, 30, 0.5f, 1 };
	KrValue m_samples{ 1, 64, 1, 0 };
	KrValue m_skyLight{ 0, 1000, 5, 0 };
	KrValue m_skyIterations{ 1, 8, 1, 0 };

	QCheckBox* m_compile;
	KrValue m_gamma{ 0.5f, 3, 0.05f, 2 };
	KrValue m_compensate{ 0.5f, 4, 0.05f, 2 };
	KrValue m_saturation{ 0, 2, 0.05f, 2 };
	KrValue m_bounce{ 0, 8, 1, 0 };
	KrValue m_lightCount{ 1, 2048, 1, 0 };
	KrValue m_engineShift{ 0, 2, 1, 0 };
	KrValue m_engineGamma{ 0.5f, 3, 0.05f, 2 };
	KrValue m_shadowedLights{ 0, 8, 1, 0 };

	static QWidget* row( std::initializer_list<QWidget*> widgets ){
		auto *w = new QWidget;
		auto *l = new QHBoxLayout( w );
		l->setContentsMargins( 0, 0, 0, 0 );
		for( QWidget* x : widgets )
			l->addWidget( x );
		return w;
	}
	void addValue( QGridLayout* grid, int& r, const char* label, KrValue& value, const char* tip, std::function<void()> changed ){
		auto *l = new QLabel( label );
		l->setToolTip( tip );
		value.slider->setToolTip( tip );
		value.spin->setToolTip( tip );
		value.m_changed = std::move( changed );
		grid->addWidget( l, r, 0 );
		grid->addWidget( value.slider, r, 1 );
		grid->addWidget( value.spin, r, 2 );
		++r;
	}
	void updateSwatch(){
		const QColor c = QColor::fromRgbF( m_r.value(), m_g.value(), m_b.value() );
		m_colourButton->setStyleSheet( QString( "background-color: %1; min-width: 48px;" ).arg( c.name() ) );
	}
	/// any sun value moved: the preview uses the calibrator values from now on
	void sunEdited(){
		g_krSun.overridden = true;
		g_krSun.valid = true;
		g_krSun.colour = Vector3( m_r.value(), m_g.value(), m_b.value() );
		g_krSun.intensity = m_intensity.value();
		g_krSun.degrees = m_degrees.value();
		g_krSun.elevation = m_elevation.value();
		g_krSun.ext = m_ext->isChecked();
		g_krSun.deviance = m_deviance.value();
		g_krSun.samples = m_samples.value();
		g_krSun.skyLight = m_skyLight.value();
		g_krSun.skyIterations = m_skyIterations.value();
		m_deviance.slider->setEnabled( g_krSun.ext );
		m_deviance.spin->setEnabled( g_krSun.ext );
		m_samples.slider->setEnabled( g_krSun.ext );
		m_samples.spin->setEnabled( g_krSun.ext );
		updateSwatch();
		sunEditedMarker();
		KrLightPreview_enable();
		KrLightPreview_redraw();
	}
	void compileEdited(){
		g_krCompile.enabled = m_compile->isChecked();
		g_krCompile.gamma = m_gamma.value();
		g_krCompile.compensate = m_compensate.value();
		g_krCompile.saturation = m_saturation.value();
		g_krCompile.bounce = int( m_bounce.value() );
		if( g_krCompile.enabled )
			KrLightPreview_enable();
		KrLightPreview_redraw();
	}

	QString sunLine() const {
		const auto n = []( float v ){ return QString::number( v, 'g', 6 ); };
		QString line = g_krSun.ext
		             ? QString( "q3map_sunExt %1 %2 %3 %4 %5 %6 %7 %8" ).arg( n( g_krSun.colour[0] ), n( g_krSun.colour[1] ), n( g_krSun.colour[2] ),
		               n( g_krSun.intensity ), n( g_krSun.degrees ), n( g_krSun.elevation ), n( g_krSun.deviance ), n( g_krSun.samples ) )
		             : QString( "q3map_sun %1 %2 %3 %4 %5 %6" ).arg( n( g_krSun.colour[0] ), n( g_krSun.colour[1] ), n( g_krSun.colour[2] ),
		               n( g_krSun.intensity ), n( g_krSun.degrees ), n( g_krSun.elevation ) );
		return line;
	}
	QString skyLine() const {
		return g_krSun.skyLight > 0 ? QString( "q3map_skyLight %1 %2" ).arg( QString::number( g_krSun.skyLight, 'g', 6 ), QString::number( int( g_krSun.skyIterations ) ) ) : QString();
	}
	QString compileFlags() const {
		const auto n = []( float v ){ return QString::number( v, 'g', 4 ); };
		QString flags;
		if( g_krCompile.bounce > 0 ) flags += QString( " -bounce %1" ).arg( g_krCompile.bounce );
		if( g_krCompile.gamma != 1 ) flags += " -gamma " + n( g_krCompile.gamma );
		if( g_krCompile.compensate != 1 ) flags += " -compensate " + n( g_krCompile.compensate );
		if( g_krCompile.saturation != 1 ) flags += " -saturation " + n( g_krCompile.saturation );
		return flags.trimmed();
	}

	/// rewrites the sun / skyLight lines of the shader in its .shader file (a .bak copy is kept)
	bool writeShader( QString& message ){
		if( g_krSun.shader.empty() ){
			message = "This map has no sky brush with a sky shader (surfaceparm sky). Copy the lines and paste them into your sky shader.";
			return false;
		}
		IShader* shader = GlobalShaderSystem().getShaderForName( g_krSun.shader.c_str() );
		const CopiedString relative( shader->getShaderFileName() );
		shader->DecRef();
		const char* root = GlobalFileSystem().findFile( relative.c_str() );
		if( string_empty( relative.c_str() ) || string_empty( root ) ){
			message = QString( "Can't find the file of shader %1." ).arg( g_krSun.shader.c_str() );
			return false;
		}
		if( !string_empty( root ) && root[string_length( root ) - 1] != '/' ){
			message = QString( "%1 is inside %2. Extract the .shader file to your base folder to edit it, or copy the lines." ).arg( relative.c_str(), root );
			return false;
		}
		const QString path = QString( root ) + relative.c_str();
		QFile file( path );
		if( !file.open( QIODevice::ReadOnly ) ){
			message = "Can't read " + path;
			return false;
		}
		QString text = QString::fromLatin1( file.readAll() );
		file.close();

		// find the shader: its name alone on a line, then the block's braces (braces inside words like {grate don't count)
		const auto isSpace = []( QChar c ){ return c.isSpace(); };
		int open = -1, close = -1;
		{
			int depth = 0, i = 0;
			bool nameMatched = false;
			while( i < text.size() ){
				if( text.mid( i, 2 ) == "//" ){
					while( i < text.size() && text[i] != '\n' ) ++i;
					continue;
				}
				if( text.mid( i, 2 ) == "/*" ){
					const int e = text.indexOf( "*/", i + 2 );
					i = e < 0 ? text.size() : e + 2;
					continue;
				}
				if( isSpace( text[i] ) ){ ++i; continue; }
				// a token
				int e = i;
				while( e < text.size() && !isSpace( text[e] ) ) ++e;
				const QString token = text.mid( i, e - i );
				if( token == "{" ){
					if( depth == 0 && nameMatched ) open = i;
					++depth;
				}
				else if( token == "}" ){
					--depth;
					if( depth == 0 && open >= 0 ){ close = i; break; }
				}
				else if( depth == 0 ){
					nameMatched = token.compare( g_krSun.shader.c_str(), Qt::CaseInsensitive ) == 0;
				}
				i = e;
			}
		}
		if( open < 0 || close < 0 ){
			message = QString( "Can't find %1 in %2." ).arg( g_krSun.shader.c_str(), path );
			return false;
		}
		// replace or insert the lines inside the block (only outside stages)
		QString block = text.mid( open + 1, close - open - 1 );
		QStringList lines = block.split( '\n' );
		bool sunDone = false, skyDone = false;
		const QString eol = text.contains( "\r\n" ) ? "\r" : ""; // keep Windows line ends
		int depth = 0;
		for( int k = 0; k < lines.size(); ++k ){
			const QString trimmed = lines[k].trimmed();
			const QString first = trimmed.section( QRegularExpression( "\\s+" ), 0, 0 );
			if( depth == 0 && ( first.compare( "q3map_sun", Qt::CaseInsensitive ) == 0 || first.compare( "q3map_sunExt", Qt::CaseInsensitive ) == 0 ) ){
				const QString indent = lines[k].left( lines[k].indexOf( first ) );
				lines[k] = sunDone ? eol : indent + KrMergeLine( trimmed, sunLine(), g_krSun.ext ? c_sunExtTolerance : c_sunTolerance ) + eol;
				sunDone = true;
			}
			else if( depth == 0 && first.compare( "q3map_skyLight", Qt::CaseInsensitive ) == 0 ){
				const QString indent = lines[k].left( lines[k].indexOf( first ) );
				lines[k] = skyLine().isEmpty() ? eol : indent + KrMergeLine( trimmed, skyLine(), c_skyTolerance ) + eol;
				skyDone = true;
			}
			if( first == "{" ) ++depth;
			if( first == "}" ) --depth;
		}
		if( !sunDone && g_krSun.intensity > 0 )
			lines.insert( 1, "\t" + sunLine() + eol );
		if( !skyDone && !skyLine().isEmpty() )
			lines.insert( 1, "\t" + skyLine() + eol );
		text = text.left( open + 1 ) + lines.join( '\n' ) + text.mid( close );

		QFile::remove( path + ".bak" );
		QFile::copy( path, path + ".bak" );
		if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ){
			message = "Can't write " + path;
			return false;
		}
		file.write( text.toLatin1() );
		file.close();
		message = QString( "Written to %1 (old version: %2.bak)." ).arg( path, relative.c_str() );
		return true;
	}

public:
	SunCalibrator( QWidget* parent ) : QDialog( parent, Qt::Tool ){
		setWindowTitle( "Sun Calibrator / Light Preview Settings" );
		auto *vbox = new QVBoxLayout( this );

		m_shaderLabel = new QLabel;
		m_shaderLabel->setWordWrap( true );
		vbox->addWidget( m_shaderLabel );

		{
			auto *group = new QGroupBox( "Sun (q3map_sun / q3map_sunExt)" );
			auto *grid = new QGridLayout( group );
			int r = 0;
			m_colourButton = new QPushButton( "Pick..." );
			m_colourButton->setToolTip( "Sun colour" );
			QObject::connect( m_colourButton, &QPushButton::clicked, [this](){
				QColorDialog dialog( QColor::fromRgbF( m_r.value(), m_g.value(), m_b.value() ), this );
				dialog.setWindowTitle( "Sun colour" );
				QObject::connect( &dialog, &QColorDialog::currentColorChanged, [this]( const QColor& c ){ // live while picking
					m_r.set( c.redF() ); m_g.set( c.greenF() ); m_b.set( c.blueF() );
					sunEdited();
				} );
				const QColor before = QColor::fromRgbF( m_r.value(), m_g.value(), m_b.value() );
				if( dialog.exec() != QDialog::Accepted ){
					m_r.set( before.redF() ); m_g.set( before.greenF() ); m_b.set( before.blueF() );
					sunEdited();
				}
			} );
			grid->addWidget( new QLabel( "Colour" ), r, 0 );
			grid->addWidget( m_colourButton, r, 1, 1, 2, Qt::AlignLeft );
			++r;
			const auto edited = [this](){ sunEdited(); };
			addValue( grid, r, "Red", m_r, "Sun colour, red (0-1)", edited );
			addValue( grid, r, "Green", m_g, "Sun colour, green (0-1)", edited );
			addValue( grid, r, "Blue", m_b, "Sun colour, blue (0-1)", edited );
			addValue( grid, r, "Intensity", m_intensity, "Sun brightness", edited );
			addValue( grid, r, "Degrees", m_degrees, "Direction the sun comes from, 0 = from +X (east), 90 = from +Y (north)", edited );
			addValue( grid, r, "Elevation", m_elevation, "Height of the sun above the horizon, 90 = straight down", edited );
			m_ext = new QCheckBox( "q3map_sunExt (soft sun: deviance + samples)" );
			QObject::connect( m_ext, &QCheckBox::toggled, edited );
			grid->addWidget( m_ext, r++, 0, 1, 3 );
			addValue( grid, r, "Deviance", m_deviance, "Size of the sun disc in degrees: softer shadow edges (preview: wider shadow blur)", edited );
			addValue( grid, r, "Samples", m_samples, "Rays q3map2 traces for the soft sun (compile time only)", edited );
			addValue( grid, r, "Sky light", m_skyLight, "q3map_skyLight amount: light from the whole sky (preview: ambient)", edited );
			addValue( grid, r, "Iterations", m_skyIterations, "q3map_skyLight iterations (compile time only)", edited );
			vbox->addWidget( group );
		}
		{
			auto *buttons = new QHBoxLayout;
			auto *reset = new QPushButton( "Reset to shader" );
			reset->setToolTip( "Forget the changes and use the sky shader's values again" );
			QObject::connect( reset, &QPushButton::clicked, [this](){ KrSun_rescan(); refresh(); } );
			auto *copy = new QPushButton( "Copy lines" );
			copy->setToolTip( "Copy the q3map_sun / q3map_skyLight lines to the clipboard" );
			QObject::connect( copy, &QPushButton::clicked, [this](){
				QString text = sunLine();
				if( !skyLine().isEmpty() ) text += '\n' + skyLine();
				QGuiApplication::clipboard()->setText( text );
				globalOutputStream() << "Sun Calibrator: copied\n" << text.toLatin1().constData() << '\n';
			} );
			auto *write = new QPushButton( "Write to shader file" );
			write->setToolTip( "Put these values into the sky shader's .shader file (a .bak copy is kept) and reload shaders" );
			QObject::connect( write, &QPushButton::clicked, [this](){
				QString message;
				if( writeShader( message ) ){
					globalOutputStream() << "Sun Calibrator: " << message.toLatin1().constData() << '\n';
					RefreshShaders();
					KrSun_rescan();
					refresh();
					QMessageBox::information( this, "Sun Calibrator", message );
				}
				else{
					globalWarningStream() << "Sun Calibrator: " << message.toLatin1().constData() << '\n';
					QMessageBox::warning( this, "Sun Calibrator", message );
				}
			} );
			buttons->addWidget( reset );
			buttons->addWidget( copy );
			buttons->addWidget( write );
			vbox->addLayout( buttons );
		}
		{
			auto *group = new QGroupBox( "Lights" );
			auto *grid = new QGridLayout( group );
			int r = 0;
			addValue( grid, r, "Lights", m_lightCount, "How many light entities (nearest to the camera) light the preview, up to 2048. Each spot only adds up the 48 strongest lights that reach it", [this](){
				g_krPreviewLightCount = int( m_lightCount.value() );
				KrLightPreview_redraw();
			} );
			addValue( grid, r, "With shadows", m_shadowedLights, "How many of those (the nearest) cast shadows, up to 8. More = slower updates when you edit. Set Lights to the same number if lights without shadow shining through walls look wrong", [this](){
				g_krShadowedLights = int( m_shadowedLights.value() );
				KrLightPreview_redraw();
			} );
			vbox->addWidget( group );
		}
		{
			auto *group = new QGroupBox( "Compile preview (approximation of q3map2 -light options)" );
			auto *grid = new QGridLayout( group );
			int r = 0;
			m_compile = new QCheckBox( "Show on the live light preview" );
			grid->addWidget( m_compile, r++, 0, 1, 3 );
			const auto edited = [this](){ compileEdited(); };
			QObject::connect( m_compile, &QCheckBox::toggled, edited );
			addValue( grid, r, "Gamma", m_gamma, "-gamma: lightmap gamma, above 1 lifts the dark parts", edited );
			addValue( grid, r, "Compensate", m_compensate, "-compensate: divides the lightmap brightness", edited );
			addValue( grid, r, "Saturation", m_saturation, "-saturation: lightmap colour saturation", edited );
			addValue( grid, r, "Bounce", m_bounce, "-bounce: radiosity passes. Preview: an even fill light only - real bounce needs a compile", edited );
			auto *buttons = new QHBoxLayout;
			auto *copy = new QPushButton( "Copy flags" );
			copy->setToolTip( "Copy the -light flags to the clipboard" );
			QObject::connect( copy, &QPushButton::clicked, [this](){
				QGuiApplication::clipboard()->setText( compileFlags() );
				globalOutputStream() << "Sun Calibrator: copied " << compileFlags().toLatin1().constData() << '\n';
			} );
			auto *preset = new QPushButton( "Make build preset" );
			preset->setToolTip( "Adds 'KenRadiant: (Sun Calibrator settings)' to the Build menu: -meta, -vis, -light -fast -super 2 -filter + these flags" );
			QObject::connect( preset, &QPushButton::clicked, [this](){
				const QString light = ( "[q3map2] -light -fast -super 2 -filter " + compileFlags() ).simplified() + " \"[MapFile]\"";
				const std::vector<CopiedString> commands{ "[q3map2] -meta \"[MapFile]\"", "[q3map2] -vis -saveprt \"[MapFile]\"", light.toLatin1().constData() };
				Build_setPreset( "KenRadiant: (Sun Calibrator settings)", commands );
				QMessageBox::information( this, "Sun Calibrator", "Build menu: 'KenRadiant: (Sun Calibrator settings)' runs\n" + light );
			} );
			buttons->addWidget( copy );
			buttons->addWidget( preset );
			grid->addLayout( buttons, r++, 0, 1, 3 );
			vbox->addWidget( group );
		}
		{
			auto *group = new QGroupBox( "Game look (how SOF2 shows the lightmaps)" );
			auto *grid = new QGridLayout( group );
			int r = 0;
			addValue( grid, r, "Overbright", m_engineShift, "r_mapOverBrightBits minus r_overBrightBits: 1 = SOF2 in fullscreen (lightmaps x2, the default look), "
			          "0 = windowed / no hardware gamma (x1, darker), 2 = x4 (washed out)", [this](){
				g_krEngineShift = int( m_engineShift.value() );
				KrLightPreview_redraw();
			} );
			addValue( grid, r, "r_gamma", m_engineGamma, "The game's r_gamma (1 = default); above 1 brightens the whole picture like in game", [this](){
				g_krEngineGamma = m_engineGamma.value();
				KrLightPreview_redraw();
			} );
			vbox->addWidget( group );
		}
		m_engineShift.set( float( g_krEngineShift ) );
		m_engineGamma.set( g_krEngineGamma );
		refresh();
	}

	/// shows the current sun (from the map, or the calibrator's own values)
	void refresh(){
		if( g_krSun.shader.empty() )
			m_shaderLabel->setText( "No sky shader with q3map_sun on this map's sky brushes. You can still try values here; copy the lines into your sky shader." );
		else
			m_shaderLabel->setText( QString( "Sky shader: <b>%1</b>%2" ).arg( g_krSun.shader.c_str(), g_krSun.overridden ? " (edited, not saved)" : "" ) );
		m_r.set( g_krSun.colour[0] ); m_g.set( g_krSun.colour[1] ); m_b.set( g_krSun.colour[2] );
		m_intensity.set( g_krSun.intensity );
		m_degrees.set( std::fmod( std::fmod( g_krSun.degrees, 360.f ) + 360.f, 360.f ) );
		m_elevation.set( g_krSun.elevation );
		{
			const QSignalBlocker b( m_ext );
			m_ext->setChecked( g_krSun.ext );
		}
		m_deviance.set( g_krSun.deviance );
		m_samples.set( g_krSun.samples );
		m_skyLight.set( g_krSun.skyLight );
		m_skyIterations.set( g_krSun.skyIterations );
		m_deviance.slider->setEnabled( g_krSun.ext ); m_deviance.spin->setEnabled( g_krSun.ext );
		m_samples.slider->setEnabled( g_krSun.ext ); m_samples.spin->setEnabled( g_krSun.ext );
		{
			const QSignalBlocker b( m_compile );
			m_compile->setChecked( g_krCompile.enabled );
		}
		m_gamma.set( g_krCompile.gamma );
		m_compensate.set( g_krCompile.compensate );
		m_saturation.set( g_krCompile.saturation );
		m_bounce.set( float( g_krCompile.bounce ) );
		m_lightCount.set( float( g_krPreviewLightCount ) );
		m_shadowedLights.set( float( g_krShadowedLights ) );
		updateSwatch();
	}
	void sunEditedMarker(){
		if( !g_krSun.shader.empty() )
			m_shaderLabel->setText( QString( "Sky shader: <b>%1</b> (edited, not saved)" ).arg( g_krSun.shader.c_str() ) );
	}
};

SunCalibrator* g_sunCalibrator = nullptr;

} // namespace

void KrSun_changed(){
	if( g_sunCalibrator != nullptr && g_sunCalibrator->isVisible() && !g_krSun.overridden )
		g_sunCalibrator->refresh();
}

void KrSunCalibrator_show(){
	// the dialog shows what the map's sky shader says
	extern void KrLightPreview_prepareSun();
	KrLightPreview_prepareSun();
	if( g_sunCalibrator == nullptr )
		g_sunCalibrator = new SunCalibrator( MainFrame_getWindow() );
	g_sunCalibrator->refresh();
	g_sunCalibrator->show();
	g_sunCalibrator->raise();
	g_sunCalibrator->activateWindow();
}
