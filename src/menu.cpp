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
#include <QtWidgets/QDialog>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtCore/QMetaObject>

// Show a pre-patch options dialog. Returns false if the user cancelled.
static bool show_patch_options_dialog(QWidget *parent, PatchOpts &opts)
{
	QDialog dlg(parent);
	dlg.setWindowTitle("Patch Options");

	auto *layout = new QVBoxLayout(&dlg);
	layout->addWidget(new QLabel("Select which patches to apply:"));

	auto *chk_markers = new QCheckBox("Inject Markers  —  writes OBS chapter markers as XMP cue points readable by Premiere");
	auto *chk_trim    = new QCheckBox("A/V Trim  —  fixes audio track length overhang so clips end cleanly in Premiere");
	auto *chk_names   = new QCheckBox("Track Names  —  copies OBS audio track labels into the MP4 handler name");
	auto *chk_date    = new QCheckBox("Creation Date  —  writes recording date into MP4 metadata");
	auto *chk_cfr     = new QCheckBox("CFR Normalization  —  converts variable frame rate to constant frame rate (not needed for NVENC/AMF/QSV)");

	chk_markers->setChecked(opts.markers);
	chk_trim   ->setChecked(opts.trim);
	chk_names  ->setChecked(opts.names);
	chk_date   ->setChecked(opts.date);
	chk_cfr    ->setChecked(opts.cfr);

	layout->addWidget(chk_markers);
	layout->addWidget(chk_trim);
	layout->addWidget(chk_names);
	layout->addWidget(chk_date);
	layout->addWidget(chk_cfr);
	layout->addSpacing(8);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	layout->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return false;

	opts.markers = chk_markers->isChecked();
	opts.trim    = chk_trim   ->isChecked();
	opts.names   = chk_names  ->isChecked();
	opts.date    = chk_date   ->isChecked();
	opts.cfr     = chk_cfr    ->isChecked();
	return true;
}

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
	tog_markers->setToolTip("Automatically writes OBS chapter markers as XMP cue points into\neach recording after it finishes, making them visible in Premiere Pro.");

	QAction *tog_trim = sub->addAction("Auto A/V Trim");
	tog_trim->setCheckable(true);
	tog_trim->setChecked(mp_get_auto_trim() != 0);
	tog_trim->setToolTip("Trims the video and audio track lengths to match the last complete frame,\nfixing the audio overhang that causes clips to end with silence in Premiere.");

	QAction *tog_names = sub->addAction("Auto Track Names");
	tog_names->setCheckable(true);
	tog_names->setChecked(mp_get_auto_names() != 0);
	tog_names->setToolTip("Copies your OBS audio track labels (set in Audio Settings) into the MP4\nhandler name so Premiere shows them as named tracks instead of Audio 1/2/3.");

	QAction *tog_date = sub->addAction("Auto Creation Date");
	tog_date->setCheckable(true);
	tog_date->setChecked(mp_get_auto_date() != 0);
	tog_date->setToolTip("Writes the recording's creation date from the MP4 header into the iTunes\nmetadata field so Premiere and media browsers display the correct capture date.");

	QAction *tog_cfr = sub->addAction("Auto CFR Normalization");
	tog_cfr->setCheckable(true);
	tog_cfr->setChecked(mp_get_auto_cfr() != 0);
	tog_cfr->setToolTip("Normalizes variable frame rate recordings to a constant frame rate.\nNot needed for hardware encoders (NVENC, AMF, QSV) which are already CFR.");

	sub->addSeparator();

	// ── Manual fixes ───────────────────────────────────────────────────────
	QAction *act_folder = sub->addAction("Patch Folder...");
	act_folder->setToolTip("Choose any recording in a folder — all MP4s (and MKVs) in that folder\nwill be patched with the options you select.");
	QAction *act_file   = sub->addAction("Patch File...");
	act_file->setToolTip("Choose one or more MP4 files to patch with the options you select.");

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

	QObject::connect(act_folder, &QAction::triggered, main_win, [main_win]() {
		PatchOpts opts{mp_get_auto_markers() != 0, mp_get_auto_trim() != 0,
		               mp_get_auto_names()   != 0, mp_get_auto_date() != 0,
		               mp_get_auto_cfr()     != 0};
		if (!show_patch_options_dialog(main_win, opts))
			return;
		mp_fix_folder_opts(opts, [](int processed) {
			QMetaObject::invokeMethod(qApp, [processed]() {
				QMessageBox::information(
					nullptr, "Premiere Patch",
					processed > 0
						? QString("Done! Patched %1 file(s).").arg(processed)
						: QString("All files already patched.\nNo changes applied."));
			}, Qt::QueuedConnection);
		});
	});

	QObject::connect(act_file, &QAction::triggered, main_win, [main_win]() {
		PatchOpts opts{mp_get_auto_markers() != 0, mp_get_auto_trim() != 0,
		               mp_get_auto_names()   != 0, mp_get_auto_date() != 0,
		               mp_get_auto_cfr()     != 0};
		if (!show_patch_options_dialog(main_win, opts))
			return;
		mp_fix_file_opts(opts, [](int processed) {
			QMetaObject::invokeMethod(qApp, [processed]() {
				QMessageBox::information(
					nullptr, "Premiere Patch",
					processed > 0
						? QString("Done! Patched %1 file(s).").arg(processed)
						: QString("All files already patched.\nNo changes applied."));
			}, Qt::QueuedConnection);
		});
	});

	// ── Append to Tools menu ───────────────────────────────────────────────
	tools_menu->addSeparator();
	tools_menu->addMenu(sub);
}

