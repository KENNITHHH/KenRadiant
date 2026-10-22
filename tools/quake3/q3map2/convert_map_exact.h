/* -------------------------------------------------------------------------------
   Exact standard-format (Quake 3 / SOF2) texture recovery for -convert -format map.

   Strategy, validated against Raven's kam6.map / kam6.bsp ground truth:
     1. RBSP brush sides carry surfaceNum -> use the linked planar surface directly
        (no guessing which triangle belongs to the side).
     2. Restrict to the verts lying on this side's polygon (safe for q3map2 meta-merged surfaces).
     3. Least-squares fit of st*size against the side's axial projection (all verts, not 3).
     4. Exact inverse of QuakeTextureVecs, preferring the representation editors write
        (positive X scale, symmetric shift range), then snap rotate/scale/shift to
        "hand-made" values only when the snapped texdef still reproduces every UV.
     5. Hidden sides (no surface) inherit rotate/scale/shift from the visible sides of the
        same brush, else the map-wide most common rotate/scale.
   ------------------------------------------------------------------------------- */
#pragma once

#include <vector>
#include <cmath>
#include <map>
#include <tuple>

namespace exactdec
{

constexpr double c_pi = 3.14159265358979323846;

struct TexDef
{
	double shift[2] = { 0, 0 };
	double rotate = 0;
	double scale[2] = { 0.5, 0.5 };
	double err = 0;         // max UV error in texels
	bool solved = false;
};

struct Stats
{
	int linked = 0;          // side solved from its linked surface
	int searched = 0;        // side solved from the legacy triangle search
	int exact = 0;           // err < tolerance
	int inexact = 0;         // solved but err >= tolerance (not representable / mismatch)
	int hiddenBrush = 0;     // hidden side inherited from same brush
	int hiddenGlobal = 0;    // hidden side used map default
	int bevelsDropped = 0;
	int missingImage = 0;
	double worstErr = 0;
};

inline Stats g_stats;
inline double g_defaultRotate = 0, g_defaultScale[2] = { 0.5, 0.5 };
inline double g_tolTexel = 0.02;

/* same axis choice as q3map2 TextureAxisFromPlane (incl. ydnar bias) */
inline void AxisFromNormal( const Vector3& n, int& sv, int& tv, double& p, double& q ){
	static const float base[6][3][3] = {
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } },
		{ { 0, 0, -1 }, { 1, 0, 0 }, { 0, -1, 0 } },
		{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } },
		{ { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
	};
	float best = 0; int bi = 0;
	for ( int i = 0; i < 6; ++i ){
		const float d = n[0] * base[i][0][0] + n[1] * base[i][0][1] + n[2] * base[i][0][2];
		if ( d > best + 0.0001f ) { best = d; bi = i; }
	}
	const float *v0 = base[bi][1], *v1 = base[bi][2];
	sv = v0[0] ? 0 : v0[1] ? 1 : 2;
	tv = v1[0] ? 0 : v1[1] ? 1 : 2;
	p = v0[sv];
	q = v1[tv];
}

/* forward projection, identical maths to map.cpp QuakeTextureVecs (in double) */
inline void Forward( int sv, int tv, double p, double q, double rotate, const double scale[2], double m[2][3] ){
	double s, c;
	if ( rotate == 0 ) { s = 0; c = 1; }
	else if ( rotate == 90 ) { s = 1; c = 0; }
	else if ( rotate == 180 ) { s = 0; c = -1; }
	else if ( rotate == 270 ) { s = -1; c = 0; }
	else { const double a = rotate * ( c_pi / 180.0 ); s = std::sin( a ); c = std::cos( a ); }
	// vecs[0] = (p,0) on (sv,tv); vecs[1] = (0,q)
	const double sx = scale[0] ? scale[0] : 1, sy = scale[1] ? scale[1] : 1;
	m[0][0] = c * p / sx;  m[0][1] = s * p / sx;  // coefficients on (sv, tv)
	m[1][0] = -s * q / sy; m[1][1] = c * q / sy;
	m[0][2] = m[1][2] = 0;
}

inline double Wrap( double x, double M ){
	double r = std::fmod( x, M );
	if ( r < 0 ) r += M;
	if ( r >= M / 2 ) r -= M;
	if ( std::fabs( r ) < 1e-6 || std::fabs( std::fabs( r ) - M ) < 1e-6 ) r = 0;
	return r;
}

inline double SnapTo( double x, double step, double tol ){
	const double y = std::round( x / step ) * step;
	return std::fabs( y - x ) <= tol ? y : x;
}

inline double ModErr( double d, double M ){
	d = std::fmod( d, M ); if ( d < 0 ) d += M;
	return std::min( d, M - d );
}

/* xyz: world-space verts (origin applied), st in texture units (already *W/H) */
inline bool Solve( const Vector3& normal, const std::vector<DoubleVector3>& xyz, const std::vector<BasicVector2<double>>& st,
                   double W, double H, TexDef& out ){
	const size_t n = xyz.size();
	if ( n < 3 ) return false;
	int sv, tv; double p, q;
	AxisFromNormal( normal, sv, tv, p, q );

	// centered least squares:  st = a*x + b*y + c
	double mx = 0, my = 0, ms = 0, mt = 0;
	for ( size_t i = 0; i < n; ++i ){ mx += xyz[i][sv]; my += xyz[i][tv]; ms += st[i][0]; mt += st[i][1]; }
	mx /= n; my /= n; ms /= n; mt /= n;
	double xx = 0, xy = 0, yy = 0, xs = 0, ys = 0, xt = 0, yt = 0;
	for ( size_t i = 0; i < n; ++i ){
		const double x = xyz[i][sv] - mx, y = xyz[i][tv] - my, s = st[i][0] - ms, t = st[i][1] - mt;
		xx += x * x; xy += x * y; yy += y * y; xs += x * s; ys += y * s; xt += x * t; yt += y * t;
	}
	const double D = xx * yy - xy * xy;
	if ( std::fabs( D ) < 1e-9 * ( xx + yy ) * ( xx + yy ) + 1e-12 ) return false;
	const double a0 = ( xs * yy - ys * xy ) / D, a1 = ( ys * xx - xs * xy ) / D;
	const double b0 = ( xt * yy - yt * xy ) / D, b1 = ( yt * xx - xt * xy ) / D;

	// invert: a = (c p, s p)/sx ; b = (-s q, c q)/sy ; prefer sx>0, then sy>0, then smaller rotation
	const double th = std::atan2( a1 * p, a0 * p ) * ( 180.0 / c_pi );
	double bestRot = 0, bestSx = 1, bestSy = 1; int bestKey = 99; double bestRotKey = 1e9;
	for ( int flip = 0; flip < 2; ++flip ){
		const double t = th + 180.0 * flip, r = t * ( c_pi / 180.0 );
		const double c = std::cos( r ), s = std::sin( r );
		const double ka = ( a0 * c + a1 * s ) * p;
		const double kb = ( -b0 * s + b1 * c ) * q;
		if ( std::fabs( ka ) < 1e-12 || std::fabs( kb ) < 1e-12 ) continue;
		const double sx = 1 / ka, sy = 1 / kb;
		const double rot = std::fmod( t + 720.0, 360.0 );
		const int key = ( sx < 0 ) * 2 + ( sy < 0 );
		if ( key < bestKey || ( key == bestKey && rot < bestRotKey ) ){
			bestKey = key; bestRotKey = rot; bestRot = rot; bestSx = sx; bestSy = sy;
		}
	}
	if ( bestKey == 99 ) return false;

	auto evaluate = [&]( double rot, double sx, double sy, TexDef& td ){
		const double sc[2] = { sx, sy };
		double m[2][3];
		Forward( sv, tv, p, q, rot, sc, m );
		double s0 = 0, t0 = 0;
		for ( size_t i = 0; i < n; ++i ){
			s0 += st[i][0] - ( m[0][0] * xyz[i][sv] + m[0][1] * xyz[i][tv] );
			t0 += st[i][1] - ( m[1][0] * xyz[i][sv] + m[1][1] * xyz[i][tv] );
		}
		s0 /= n; t0 /= n;
		double sh[2] = { Wrap( s0, W ), Wrap( t0, H ) };
		for ( int k = 0; k < 2; ++k ){
			const double M = k ? H : W;
			for ( double step : { 1.0, 0.5, 0.125, 0.001 } ){
				const double y = SnapTo( sh[k], step, 0.01 );
				if ( y != sh[k] ) { sh[k] = Wrap( y, M ); break; }
			}
		}
		double e = 0;
		for ( size_t i = 0; i < n; ++i ){
			const double ps = m[0][0] * xyz[i][sv] + m[0][1] * xyz[i][tv] + sh[0];
			const double pt = m[1][0] * xyz[i][sv] + m[1][1] * xyz[i][tv] + sh[1];
			e = std::max( e, std::max( ModErr( ps - st[i][0], W ), ModErr( pt - st[i][1], H ) ) );
		}
		td.shift[0] = sh[0]; td.shift[1] = sh[1]; td.rotate = rot; td.scale[0] = sx; td.scale[1] = sy; td.err = e; td.solved = true;
	};

	// candidate snaps
	std::vector<double> rots{ bestRot, SnapTo( bestRot, 90, 0.002 ), SnapTo( bestRot, 1, 0.002 ), SnapTo( bestRot, 0.01, 0.006 ) };
	auto scaleCands = []( double s ){
		std::vector<double> v{ s };
		for ( double step : { 0.125, 0.0625, 0.03125, 0.001, 0.000001 } )
			v.push_back( SnapTo( s, step, 1e-5 ) );
		return v;
	};
	const auto sxs = scaleCands( bestSx ), sys = scaleCands( bestSy );
	auto niceness = []( const TexDef& td ){
		auto frac = []( double x, double st ){ return std::fabs( x / st - std::round( x / st ) ) > 1e-9; };
		return int( frac( td.rotate, 1 ) ) + frac( td.scale[0], 0.03125 ) + frac( td.scale[1], 0.03125 )
		     + frac( td.shift[0], 1 ) + frac( td.shift[1], 1 );
	};
	bool have = false; TexDef best;
	for ( double r : rots ) for ( double sx : sxs ) for ( double sy : sys ){
		TexDef td;
		evaluate( std::fmod( r + 360.0, 360.0 ), sx, sy, td );
		if ( !have ){ best = td; have = true; continue; }
		const bool okA = td.err <= g_tolTexel, okB = best.err <= g_tolTexel;
		if ( okA != okB ){ if ( okA ) best = td; continue; }
		const int na = niceness( td ), nb = niceness( best );
		if ( na < nb || ( na == nb && td.err < best.err ) ) best = td;
	}
	if ( best.rotate == 360 ) best.rotate = 0;
	out = best;
	return true;
}

/* compact number printing: integers without decimals, else up to 6 decimals trimmed */
inline const char *Num( double v, char *buf ){
	if ( std::fabs( v - std::round( v ) ) < 1e-6 ) { snprintf( buf, 32, "%d", int( std::lround( v ) ) ); return buf; }
	snprintf( buf, 32, "%.6f", v );
	char *e = buf + strlen( buf ) - 1;
	while ( *e == '0' ) *e-- = 0;
	if ( *e == '.' ) *e = 0;
	return buf;
}

} // namespace exactdec
