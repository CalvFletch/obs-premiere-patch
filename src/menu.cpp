/*
 * obs-premiere-patch  —  menu.cpp
 *
 * Builds the "Premiere Patch" submenu inside OBS's Tools menu.
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
		        "[obs-premiere-patch] mp_setup_menu: no main window");
		return;
	}

	QMenu *tools_menu = main_win->findChild<QMenu *>("menuTools");
	if (!tools_menu) {
		obs_log(LOG_WARNING,
		        "[obs-premiere-patch] mp_setup_menu: "
		        "Tools menu not found");
		return;
	}

	QMenu *sub = new QMenu("Premiere Patch", main_win);

	// ── Toggles ────────────────────────────────────────────────────────────
	QAction *tog_markers = sub->addAction("Auto-Inject Markers");
	tog_markers->setCheckable(true);
	tog_markers->setChecked(mp_get_auto_markers() != 0);

	QAction *tog_trim = sub->addAction("Auto A/V Trim");
	tog_trim->setCheckable(true);
	tog_trim->setChecked(mp_get_auto_trim() != 0);

	QAction *tog_names = sub->addAction("Auto Track Names");
	tog_names->setCheckable(true);
	tog_names->setChecked(mp_get_auto_names() != 0);

	QAction *tog_date = sub->addAction("Auto Creation Date");
	tog_date->setCheckable(true);
	tog_date->setChecked(mp_get_auto_date() != 0);

	QAction *tog_cfr = sub->addAction("Auto CFR Normalization");
	tog_cfr->setCheckable(true);
	tog_cfr->setChecked(mp_get_auto_cfr() != 0);

	sub->addSeparator();

	// ── Manual fixes ───────────────────────────────────────────────────────
	QAction *act_folder = sub->addAction("Patch Folder...");
	QAction *act_file   = sub->addAction("Patch File...");

	// ── Connections ────────────────────────────────────────────────────────
	QObject::connect(tog_markers, &QAction::toggled,
	                 [](bool on) { mp_set_auto_markers(on ? 1 : 0); });
	QObject::connect(tog_trim, &QAction::toggled,
	                 [](bool on) { mp_set_auto_trim(on ? 1 : 0); });
	QObject::connect(tog_names, &QAction::toggled,
	                 [](bool on) { mp_set_auto_names(on ? 1 : 0); });
	QObject::connect(tog_date, &QAction::toggled,
	                 [](bool on) { mp_set_auto_date(on ? 1 : 0); });
	QObject::connect(tog_cfr, &QAction::toggled,
	                 [](bool on) { mp_set_auto_cfr(on ? 1 : 0); });
	QObject::connect(act_folder, &QAction::triggered,
	                 []() { mp_fix_folder(); });
	QObject::connect(act_file, &QAction::triggered,
	                 []() { mp_fix_file(); });

	// ── Append to Tools menu ───────────────────────────────────────────────
	tools_menu->addSeparator();
	tools_menu->addMenu(sub);
}
