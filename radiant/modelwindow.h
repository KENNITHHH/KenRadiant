/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once


void ModelBrowser_Construct();
void ModelBrowser_Destroy();


class QWidget* ModelBrowser_constructWindow( QWidget* toplevel );
void ModelBrowser_destroyWindow();

void ModelBrowser_flushReferences();

#include "math/vectorfwd.h"

const Vector3& ModelBrowser_getBackgroundColour();
void ModelBrowser_setBackgroundColour( const Vector3& colour );

// KenRadiant: drag a model from the model browser into the 2D/3D views (live placement, one undo step)
constexpr const char* c_modelDropMime = "application/x-kenradiant-model";
void ModelDrop_begin( const char* modelPath, const Vector3& origin );
void ModelDrop_move( const Vector3& origin );
void ModelDrop_end( bool keep );
bool ModelDrop_active();
void ModelDrop_park(); // moves the dragged model out of the way so the placement ray does not hit it
