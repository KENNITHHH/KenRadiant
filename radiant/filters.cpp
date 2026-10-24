/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

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

#include "filters.h"

#include "debugging/debugging.h"

#include "ifilter.h"

#include "iscenegraph.h"

#include <list>
#include <set>

#include "gtkutil/widget.h"
#include "gtkmisc.h"
#include "commands.h"
#include "preferences.h"

struct filters_globals_t
{
	std::size_t exclude = 0;
};

filters_globals_t g_filters_globals;

inline bool filter_active( int mask ){
	return ( g_filters_globals.exclude & mask ) > 0;
}

class FilterWrapper
{
public:
	FilterWrapper( Filter& filter, int mask ) : m_filter( filter ), m_mask( mask ){
	}
	void update(){
		m_filter.setActive( filter_active( m_mask ) );
	}
private:
	Filter& m_filter;
	int m_mask;
};

typedef std::list<FilterWrapper> Filters;
Filters g_filters;

typedef std::set<Filterable*> Filterables;
Filterables g_filterables;

void UpdateFilters(){
	for ( auto& filter : g_filters )
	{
		filter.update();
	}

	for ( auto *filterable : g_filterables )
	{
		filterable->updateFiltered();
	}
}


class BasicFilterSystem : public FilterSystem
{
public:
	void addFilter( Filter& filter, int mask ) override {
		g_filters.push_back( FilterWrapper( filter, mask ) );
		g_filters.back().update();
	}
	void registerFilterable( Filterable& filterable ) override {
		filterable.updateFiltered();
		const bool inserted = g_filterables.insert( &filterable ).second;
		ASSERT_MESSAGE( inserted, "filterable already registered" );
	}
	void unregisterFilterable( Filterable& filterable ) override {
		const bool erased = g_filterables.erase( &filterable );
		ASSERT_MESSAGE( erased, "filterable not registered" );
	}
};

BasicFilterSystem g_FilterSystem;

FilterSystem& GetFilterSystem(){
	return g_FilterSystem;
}

void PerformFiltering(){
	UpdateFilters();
	SceneChangeNotify();
}

class ToggleFilterFlag
{
	const unsigned int m_mask;
public:
	ToggleItem m_item;

	ToggleFilterFlag( unsigned int mask ) : m_mask( mask ), m_item( ActiveCaller( *this ) ){
	}
	ToggleFilterFlag( const ToggleFilterFlag& other ) : m_mask( other.m_mask ), m_item( ActiveCaller( *this ) ){
	}
	void active( const BoolImportCallback& importCallback ){
		importCallback( ( g_filters_globals.exclude & m_mask ) != 0 );
	}
	typedef MemberCaller<ToggleFilterFlag, void(const BoolImportCallback&), &ToggleFilterFlag::active> ActiveCaller;
	void toggle(){
		g_filters_globals.exclude ^= m_mask;
		m_item.update();
		PerformFiltering();
	}
	void reset(){
		g_filters_globals.exclude = 0;
		m_item.update();
		PerformFiltering();
	}
	typedef MemberCaller<ToggleFilterFlag, void(), &ToggleFilterFlag::toggle> ToggleCaller;
};


typedef std::list<ToggleFilterFlag> ToggleFilterFlags;
ToggleFilterFlags g_filter_items;

void add_filter_command( unsigned int flag, const char* command, const QKeySequence& accelerator = {} ){
	g_filter_items.push_back( ToggleFilterFlag( flag ) );
	GlobalToggles_insert( command, ToggleFilterFlag::ToggleCaller( g_filter_items.back() ), ToggleItem::AddCallbackCaller( g_filter_items.back().m_item ), accelerator );
}

/// KenRadiant: Unreal-style game view (Shift+G) - hides editor-only stuff (tool brushes, triggers, lights, paths, portals),
/// toggling it again restores exactly the filters that were set before
static bool g_gameView = false;
static unsigned int g_gameView_savedExclude = 0;
static bool g_gameView_savedSkybox = false;
void SkyboxPreview_set( bool on );
bool SkyboxPreview_get();
ToggleItem g_gameView_item{ BoolExportCaller( g_gameView ) };
#include "ientity.h"
static bool g_gameView_names, g_gameView_targetNames, g_gameView_angles, g_gameView_connections, g_gameView_radii;
/// the editor closes in Game View: save the real filter / display settings, not Game View's
void GameView_beforeShutdown(){
	if( !g_gameView )
		return;
	g_gameView = false;
	g_filters_globals.exclude = g_gameView_savedExclude;
	EntityCreator& entities = GlobalEntityCreator();
	entities.setShowNames( g_gameView_names ); entities.setShowTargetNames( g_gameView_targetNames ); entities.setShowAngles( g_gameView_angles );
	entities.setShowConnections( g_gameView_connections ); entities.setLightRadii( g_gameView_radii );
}
bool GameView_on(){ return g_gameView; } // KenRadiant Lighting Probe
void GameView_toggle(){
	constexpr unsigned int editorOnly = EXCLUDE_CAULK | EXCLUDE_CLIP | EXCLUDE_HINTSSKIPS | EXCLUDE_TRIGGERS | EXCLUDE_LIGHTS | EXCLUDE_PATHS
	                                  | EXCLUDE_CLUSTERPORTALS | EXCLUDE_AREAPORTALS | EXCLUDE_BOTCLIP | EXCLUDE_LIGHTGRID | EXCLUDE_VISPORTALS
	                                  | EXCLUDE_NODRAW | EXCLUDE_POINT_ENT; // point entities = sounds, info_*, targets (models and lights have their own filters)
	g_gameView = !g_gameView;
	// entity names, angles, target lines and light radii are editor-only too
	bool& names = g_gameView_names; bool& targetNames = g_gameView_targetNames; bool& angles = g_gameView_angles;
	bool& connections = g_gameView_connections; bool& radii = g_gameView_radii;
	EntityCreator& entities = GlobalEntityCreator();
	if( g_gameView ){
		g_gameView_savedExclude = g_filters_globals.exclude;
		g_filters_globals.exclude |= editorOnly;
		g_gameView_savedSkybox = SkyboxPreview_get();
		SkyboxPreview_set( true ); // the real sky, like in game
		names = entities.getShowNames(); targetNames = entities.getShowTargetNames(); angles = entities.getShowAngles();
		connections = entities.getShowConnections(); radii = entities.getLightRadii();
		entities.setShowNames( false ); entities.setShowTargetNames( false ); entities.setShowAngles( false );
		entities.setShowConnections( false ); entities.setLightRadii( false );
	}
	else{
		g_filters_globals.exclude = g_gameView_savedExclude;
		SkyboxPreview_set( g_gameView_savedSkybox );
		entities.setShowNames( names ); entities.setShowTargetNames( targetNames ); entities.setShowAngles( angles );
		entities.setShowConnections( connections ); entities.setLightRadii( radii );
	}
	for ( auto& toggle : g_filter_items )
		toggle.m_item.update();
	g_gameView_item.update();
	PerformFiltering();
}

void InvertFilters(){
	for ( auto& toggle : g_filter_items )
	{
		toggle.toggle();
	}
}

void ResetFilters(){
	for ( auto& toggle : g_filter_items )
	{
		toggle.reset();
	}
}

void Filters_constructMenu( QMenu* menu ){
	menu->addSection( "Checked = hidden" ); // KenRadiant
	create_check_menu_item_with_mnemonic( menu, "World", "FilterWorldBrushes" );
	create_check_menu_item_with_mnemonic( menu, "Entities", "FilterEntities" );
	if ( g_pGameDescription->mGameType == "doom3" ) {
		create_check_menu_item_with_mnemonic( menu, "Visportals", "FilterVisportals" );
	}
	else
	{
		create_check_menu_item_with_mnemonic( menu, "Areaportals", "FilterAreaportals" );
	}
	create_check_menu_item_with_mnemonic( menu, "Translucent", "FilterTranslucent" );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		create_check_menu_item_with_mnemonic( menu, "Liquids", "FilterLiquids" );
	}
	create_check_menu_item_with_mnemonic( menu, "Caulk", "FilterCaulk" );
	create_check_menu_item_with_mnemonic( menu, "Clips", "FilterClips" );
	create_check_menu_item_with_mnemonic( menu, "Paths", "FilterPaths" );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		create_check_menu_item_with_mnemonic( menu, "Clusterportals", "FilterClusterportals" );
	}
	create_check_menu_item_with_mnemonic( menu, "Lights", "FilterLights" );
	create_check_menu_item_with_mnemonic( menu, "Structural", "FilterStructural" );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		create_check_menu_item_with_mnemonic( menu, "Lightgrid", "FilterLightgrid" );
	}
	create_check_menu_item_with_mnemonic( menu, "Patches", "FilterPatches" );
	create_check_menu_item_with_mnemonic( menu, "Details", "FilterDetails" );
	create_check_menu_item_with_mnemonic( menu, "Hints", "FilterHintsSkips" );
	create_check_menu_item_with_mnemonic( menu, "Models", "FilterModels" );
	create_check_menu_item_with_mnemonic( menu, "Triggers", "FilterTriggers" );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		create_check_menu_item_with_mnemonic( menu, "Botclips", "FilterBotClips" );
		create_check_menu_item_with_mnemonic( menu, "Decals", "FilterDecals" );
	}
	create_check_menu_item_with_mnemonic( menu, "FuncGroups", "FilterFuncGroups" );
	create_check_menu_item_with_mnemonic( menu, "Point Entities", "FilterPointEntities" );
	create_check_menu_item_with_mnemonic( menu, "Sky", "FilterSky" );
	create_check_menu_item_with_mnemonic( menu, "Nodraw (all tool surfaces)", "FilterNodraw" ); // KenRadiant
	// filter manipulation
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Invert filters", "InvertFilters" );
	create_menu_item_with_mnemonic( menu, "Reset filters", "ResetFilters" );
	menu->addSeparator();
	create_check_menu_item_with_mnemonic( menu, "Game View (hide editor-only)", "ToggleGameView" ); // KenRadiant
}


#include "preferencesystem.h"
#include "stringio.h"
#include "dialog.h"

void ConstructFilters(){
	GlobalPreferenceSystem().registerPreference( "SI_Exclude", SizeImportStringCaller( g_filters_globals.exclude ), SizeExportStringCaller( g_filters_globals.exclude ) );

	GlobalCommands_insert( "InvertFilters", makeCallbackF( InvertFilters ) );
	GlobalCommands_insert( "ResetFilters", makeCallbackF( ResetFilters ) );
	GlobalToggles_insert( "ToggleGameView", makeCallbackF( GameView_toggle ), ToggleItem::AddCallbackCaller( g_gameView_item ), QKeySequence( "Shift+G" ) ); // KenRadiant

	add_filter_command( EXCLUDE_WORLD, "FilterWorldBrushes", QKeySequence( "Alt+1" ) );
	add_filter_command( EXCLUDE_ENT, "FilterEntities", QKeySequence( "Alt+2" ) );
	if ( g_pGameDescription->mGameType == "doom3" ) {
		add_filter_command( EXCLUDE_VISPORTALS, "FilterVisportals", QKeySequence( "Alt+3" ) );
	}
	else
	{
		add_filter_command( EXCLUDE_AREAPORTALS, "FilterAreaportals", QKeySequence( "Alt+3" ) );
	}
	add_filter_command( EXCLUDE_TRANSLUCENT, "FilterTranslucent", QKeySequence( "Alt+4" ) );
	add_filter_command( EXCLUDE_LIQUIDS, "FilterLiquids", QKeySequence( "Alt+5" ) );
	add_filter_command( EXCLUDE_CAULK, "FilterCaulk", QKeySequence( "Alt+6" ) );
	add_filter_command( EXCLUDE_CLIP, "FilterClips", QKeySequence( "Alt+7" ) );
	add_filter_command( EXCLUDE_PATHS, "FilterPaths", QKeySequence( "Alt+8" ) );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		add_filter_command( EXCLUDE_CLUSTERPORTALS, "FilterClusterportals", QKeySequence( "Alt+9" ) );
	}
	add_filter_command( EXCLUDE_LIGHTS, "FilterLights", QKeySequence( "Alt+0" ) );
	add_filter_command( EXCLUDE_STRUCTURAL, "FilterStructural", QKeySequence( "Ctrl+Shift+D" ) );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		add_filter_command( EXCLUDE_LIGHTGRID, "FilterLightgrid" );
	}
	add_filter_command( EXCLUDE_CURVES, "FilterPatches", QKeySequence( "Ctrl+P" ) );
	add_filter_command( EXCLUDE_DETAILS, "FilterDetails", QKeySequence( "Ctrl+D" ) );
	add_filter_command( EXCLUDE_HINTSSKIPS, "FilterHintsSkips", QKeySequence( "Ctrl+H" ) );
	add_filter_command( EXCLUDE_MODELS, "FilterModels", QKeySequence( "Shift+M" ) );
	add_filter_command( EXCLUDE_TRIGGERS, "FilterTriggers", QKeySequence( "Ctrl+Shift+T" ) );
	if ( g_pGameDescription->mGameType != "doom3" ) {
		add_filter_command( EXCLUDE_BOTCLIP, "FilterBotClips", QKeySequence( "Alt+M" ) );
		add_filter_command( EXCLUDE_DECALS, "FilterDecals", QKeySequence( "Shift+D" ) );
	}
	add_filter_command( EXCLUDE_FUNC_GROUPS, "FilterFuncGroups" );
	add_filter_command( EXCLUDE_POINT_ENT, "FilterPointEntities" );
	add_filter_command( EXCLUDE_SKY, "FilterSky" );
	add_filter_command( EXCLUDE_NODRAW, "FilterNodraw" ); // KenRadiant

	PerformFiltering();
}

void DestroyFilters(){
	g_filters.clear();
}

#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class FilterAPI
{
	FilterSystem* m_filter;
public:
	typedef FilterSystem Type;
	STRING_CONSTANT( Name, "*" );

	FilterAPI(){
		ConstructFilters();

		m_filter = &GetFilterSystem();
	}
	~FilterAPI(){
		DestroyFilters();
	}
	FilterSystem* getTable(){
		return m_filter;
	}
};

typedef SingletonModule<FilterAPI> FilterModule;
typedef Static<FilterModule> StaticFilterModule;
StaticRegisterModule staticRegisterFilter( StaticFilterModule::instance() );
