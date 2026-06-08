/*
 * obs-marker-patch  —  menu.cpp
 *
 * Builds the "Marker Patch" submenu inside OBS's Tools menu.
 * Called once from obs_module_load via mp_setup_menu().
 */

#include "marker-patch.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QtGui/QAction>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>

void mp_setup_menu(void)
{
	QMainWindow *main_win = reinterpret_cast<QMainWindow *>(
		obs_frontend_get_main_window());
	if (!main_win) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] mp_setup_menu: no main window");
		return;
	}

	QMenu *tools_menu = main_win->findChild<QMenu *>("menuTools");
	if (!tools_menu) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] mp_setup_menu: "
		        "Tools menu not found");
		return;
	}

	QMenu *sub = new QMenu("Marker Patch", main_win);

	// ── Toggles ────────────────────────────────────────────────────────────
	QAction *tog_markers = sub->addAction("Auto-Inject Markers");
	tog_markers->setCheckable(true);
	tog_markers->setChecked(mp_get_auto_markers() != 0);

	QAction *tog_trim = sub->addAction("Auto A/V Trim");
	tog_trim->setCheckable(true);
	tog_trim->setChecked(mp_get_auto_trim() != 0);

	sub->addSeparator();

	// ── Manual fixes ───────────────────────────────────────────────────────
	QAction *act_folder_markers =
		sub->addAction("Fix Folder for Markers...");
	QAction *act_file_markers = sub->addAction("Fix File for Markers...");

	sub->addSeparator();

	QAction *act_folder_trim =
		sub->addAction("Fix Folder for A/V Trim...");
	QAction *act_file_trim = sub->addAction("Fix File for A/V Trim...");

	// ── Connections ────────────────────────────────────────────────────────
	QObject::connect(tog_markers, &QAction::toggled,
	                 [](bool on) { mp_set_auto_markers(on ? 1 : 0); });
	QObject::connect(tog_trim, &QAction::toggled,
	                 [](bool on) { mp_set_auto_trim(on ? 1 : 0); });
	QObject::connect(act_folder_markers, &QAction::triggered,
	                 []() { mp_fix_folder_markers(); });
	QObject::connect(act_file_markers, &QAction::triggered,
	                 []() { mp_fix_file_markers(); });
	QObject::connect(act_folder_trim, &QAction::triggered,
	                 []() { mp_fix_folder_trim(); });
	QObject::connect(act_file_trim, &QAction::triggered,
	                 []() { mp_fix_file_trim(); });

	// ── Append to Tools menu ───────────────────────────────────────────────
	tools_menu->addSeparator();
	tools_menu->addMenu(sub);
}
