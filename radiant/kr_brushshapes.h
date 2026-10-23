#pragma once
// KenRadiant: Unreal-style brush geometry presets (box, cone, cylinder, stairs, sphere)
#include "math/vector.h"
#include <cstddef>

struct KrBrushShape
{
	enum Type { eBox, eCone, eCylinder, eCurvedStair, eLinearStair, eSpiralStair, eSphere, eCount };
	int type = eBox;
	// box
	float x = 256, y = 256, z = 256;
	bool hollow = false;
	float wall = 16;
	// cone / cylinder
	float height = 256, capHeight = 192, outerRadius = 128, innerRadius = 96;
	int sides = 8;
	// stairs
	float stairInnerRadius = 128, stepHeight = 16, stepWidth = 128, stepLength = 32, stepThickness = 16, curveAngle = 90, addToFirstStep = 0;
	int numSteps = 8, stepsPer360 = 16;
	bool counterClockwise = false, slopedCeiling = false, slopedFloor = false;
	// sphere
	float sphereRadius = 128;
	int tessellation = 1;
};

/// builds the shape as new selected worldspawn brushes; origin = bottom centre; returns the number of brushes
std::size_t KrBrushShape_build( const KrBrushShape& shape, const Vector3& origin, const char* shader );
