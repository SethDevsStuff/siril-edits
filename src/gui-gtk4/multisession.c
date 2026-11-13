/*
 * This file is part of Siril, an astronomy image processor.
 * Copyright (C) 2005-2011 Francois Meyer (dulle at free.fr)
 * Copyright (C) 2012-2026 team free-astro (see more in AUTHORS file)
 * Reference site is https://siril.org
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Siril is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Siril. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gtk/gtk.h>

#include "core/command_line_processor.h"
#include "core/gui_iface.h"
#include "core/processing.h"
#include "core/proto.h"
#include "core/siril.h"
#include "core/siril_log.h"
#include "filters/cosmetic_correction.h"
#include "gui-gtk4/file_browser.h"
#include "gui-gtk4/message_dialog.h"
#include "gui-gtk4/multisession.h"
#include "gui-gtk4/utils.h"
#include "io/conversion.h"
#include "io/FITS_symlink.h"
#include "io/image_format_fits.h"
#include "io/path_parse.h"
#include "io/sequence.h"
#include "io/single_image.h"

#define MAX_SUBSESSIONS 16

typedef struct {
	gchar *name;
	gchar *dark_path;
	gchar *flat_path;
	gchar *bias_path;
	gchar *existing_seq;
	gboolean use_existing_seq;

	// Advanced calibration options
	int dark_optim; // 0: None, 1: Auto (-opt), 2: Exposure (-opt=exp)
	gboolean equalize_cfa;
	gboolean fix_xtrans;
	gboolean calibrate_all;

	// Cosmetic correction
	gboolean cosme_enabled;
	gboolean cosme_cfa;
	int cosme_method; // 0: Dark, 1: BPM
	gboolean cosme_cold;
	double cosme_sig_cold;
	gboolean cosme_hot;
	double cosme_sig_hot;
	gchar *cosme_bpm_path;
} subsession_info_t;

typedef struct {
	gchar *full_path;
	gchar *basename;
	int subsession_idx;
	GtkWidget *row_box;
	GtkWidget *check_btn;
	GtkWidget *lbl_badge;
} light_file_item_t;

struct multisession_calib_ui {
	GtkWidget *dialog;
	gboolean closing;

	// Subsessions
	subsession_info_t subsessions[MAX_SUBSESSIONS];
	int num_subsessions;
	int current_subsession;

	// Subsession config widgets
	GtkWidget *combo_subsession;
	GtkWidget *entry_session_name;
	GtkWidget *entry_dark;
	GtkWidget *entry_flat;
	GtkWidget *entry_bias;
	GtkWidget *lbl_assigned_count;
	GtkWidget *check_use_existing;
	GtkWidget *combo_existing_seq;

	// Advanced calibration widgets
	GtkWidget *combo_dark_optim;
	GtkWidget *check_equalize_cfa;
	GtkWidget *check_fix_xtrans;
	GtkWidget *check_calibrate_all;

	// Cosmetic correction widgets
	GtkWidget *check_cosme_enable;
	GtkWidget *check_cosme_cfa;
	GtkWidget *combo_cosme_method;
	GtkWidget *box_cosme_dark;
	GtkWidget *check_cosme_cold;
	GtkWidget *spin_cosme_cold;
	GtkWidget *check_cosme_hot;
	GtkWidget *spin_cosme_hot;
	GtkWidget *btn_cosme_eval;
	GtkWidget *lbl_cosme_eval_res;
	GtkWidget *box_cosme_bpm;
	GtkWidget *entry_cosme_bpm;
	GtkWidget *btn_browse_bpm;

	GtkWidget *btn_apply_to_all;

	// Lights list
	GList *files_list; // list of light_file_item_t*
	GtkWidget *vbox_files;
	GtkWidget *lbl_total_files;
	GtkWidget *combo_assign_target;

	// Global settings
	GtkWidget *check_debayer;
	GtkWidget *check_auto_merge;
	GtkWidget *entry_combined_name;
};

static void update_subsessions_dropdown(struct multisession_calib_ui *ui);
static void refresh_subsession_ui(struct multisession_calib_ui *ui);
static void save_current_subsession_fields(struct multisession_calib_ui *ui);
static void refresh_files_list_display(struct multisession_calib_ui *ui);
static void update_cosme_controls_sensitivity(struct multisession_calib_ui *ui);
static void multisession_dialog_close(struct multisession_calib_ui *ui);

static void init_subsession(subsession_info_t *sub, int idx) {
	sub->name = g_strdup_printf("Session %d", idx + 1);
	sub->dark_path = g_strdup("");
	sub->flat_path = g_strdup("");
	sub->bias_path = g_strdup("");
	sub->existing_seq = g_strdup("");
	sub->use_existing_seq = FALSE;
	sub->dark_optim = 0;
	sub->equalize_cfa = FALSE;
	sub->fix_xtrans = FALSE;
	sub->calibrate_all = FALSE;
	sub->cosme_enabled = FALSE;
	sub->cosme_cfa = TRUE;
	sub->cosme_method = 0;
	sub->cosme_cold = TRUE;
	sub->cosme_sig_cold = 3.0;
	sub->cosme_hot = TRUE;
	sub->cosme_sig_hot = 3.0;
	sub->cosme_bpm_path = g_strdup("");
}

static void free_subsession_fields(subsession_info_t *sub) {
	if (!sub) return;
	g_free(sub->name);
	g_free(sub->dark_path);
	g_free(sub->flat_path);
	g_free(sub->bias_path);
	g_free(sub->existing_seq);
	g_free(sub->cosme_bpm_path);
	memset(sub, 0, sizeof(subsession_info_t));
}

static void free_light_item(light_file_item_t *item) {
	if (!item) return;
	g_free(item->full_path);
	g_free(item->basename);
	free(item);
}

static void on_subsession_combo_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	if (!ui->combo_subsession || !GTK_IS_DROP_DOWN(ui->combo_subsession)) return;
	GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(ui->combo_subsession));
	if (!model || !G_IS_LIST_MODEL(model)) return;
	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_subsession));
	if (selected == GTK_INVALID_LIST_POSITION) return;
	int sel = (int)selected;
	if (sel >= 0 && sel < ui->num_subsessions && sel != ui->current_subsession) {
		save_current_subsession_fields(ui);
		ui->current_subsession = sel;
		refresh_subsession_ui(ui);
	}
}

static void save_current_subsession_fields(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	if (ui->current_subsession < 0 || ui->current_subsession >= ui->num_subsessions)
		return;
	subsession_info_t *s = &ui->subsessions[ui->current_subsession];

	const char *name = gtk_editable_get_text(GTK_EDITABLE(ui->entry_session_name));
	g_free(s->name);
	s->name = g_strdup(name ? name : "");

	const char *dark = gtk_editable_get_text(GTK_EDITABLE(ui->entry_dark));
	g_free(s->dark_path);
	s->dark_path = g_strdup(dark ? dark : "");

	const char *flat = gtk_editable_get_text(GTK_EDITABLE(ui->entry_flat));
	g_free(s->flat_path);
	s->flat_path = g_strdup(flat ? flat : "");

	const char *bias = gtk_editable_get_text(GTK_EDITABLE(ui->entry_bias));
	g_free(s->bias_path);
	s->bias_path = g_strdup(bias ? bias : "");

	s->use_existing_seq = siril_toggle_get_active(ui->check_use_existing);

	// Advanced settings
	if (ui->combo_dark_optim && GTK_IS_DROP_DOWN(ui->combo_dark_optim)) {
		guint d_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_dark_optim));
		s->dark_optim = (d_sel == GTK_INVALID_LIST_POSITION) ? 0 : (int)d_sel;
	}
	s->equalize_cfa = siril_toggle_get_active(ui->check_equalize_cfa);
	s->fix_xtrans = siril_toggle_get_active(ui->check_fix_xtrans);
	s->calibrate_all = siril_toggle_get_active(ui->check_calibrate_all);

	// Cosmetic correction
	s->cosme_enabled = siril_toggle_get_active(ui->check_cosme_enable);
	s->cosme_cfa = siril_toggle_get_active(ui->check_cosme_cfa);
	if (ui->combo_cosme_method && GTK_IS_DROP_DOWN(ui->combo_cosme_method)) {
		guint c_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_cosme_method));
		s->cosme_method = (c_sel == GTK_INVALID_LIST_POSITION) ? 0 : (int)c_sel;
	}
	s->cosme_cold = siril_toggle_get_active(ui->check_cosme_cold);
	s->cosme_sig_cold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->spin_cosme_cold));
	s->cosme_hot = siril_toggle_get_active(ui->check_cosme_hot);
	s->cosme_sig_hot = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->spin_cosme_hot));

	const char *bpm = gtk_editable_get_text(GTK_EDITABLE(ui->entry_cosme_bpm));
	g_free(s->cosme_bpm_path);
	s->cosme_bpm_path = g_strdup(bpm ? bpm : "");
}

static int count_files_for_subsession(struct multisession_calib_ui *ui, int sub_idx) {
	if (!ui || ui->closing) return 0;
	int count = 0;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->subsession_idx == sub_idx) count++;
	}
	return count;
}

static void update_cosme_controls_sensitivity(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	if (!ui->check_cosme_enable || !GTK_IS_CHECK_BUTTON(ui->check_cosme_enable)) return;
	if (!ui->combo_cosme_method || !GTK_IS_DROP_DOWN(ui->combo_cosme_method)) return;
	GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(ui->combo_cosme_method));
	if (!model || !G_IS_LIST_MODEL(model)) return;

	gboolean enabled = siril_toggle_get_active(ui->check_cosme_enable);
	gtk_widget_set_sensitive(ui->check_cosme_cfa, enabled);
	gtk_widget_set_sensitive(ui->combo_cosme_method, enabled);
	gtk_widget_set_sensitive(ui->box_cosme_dark, enabled);
	gtk_widget_set_sensitive(ui->box_cosme_bpm, enabled);

	guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_cosme_method));
	int method = (selected == GTK_INVALID_LIST_POSITION) ? 0 : (int)selected;
	gtk_widget_set_visible(ui->box_cosme_dark, method == 0);
	gtk_widget_set_visible(ui->box_cosme_bpm, method == 1);

	gboolean cold_act = siril_toggle_get_active(ui->check_cosme_cold);
	gtk_widget_set_sensitive(ui->spin_cosme_cold, enabled && cold_act);

	gboolean hot_act = siril_toggle_get_active(ui->check_cosme_hot);
	gtk_widget_set_sensitive(ui->spin_cosme_hot, enabled && hot_act);
}

static void refresh_subsession_ui(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	if (ui->current_subsession < 0 || ui->current_subsession >= ui->num_subsessions)
		return;
	subsession_info_t *s = &ui->subsessions[ui->current_subsession];

	gtk_editable_set_text(GTK_EDITABLE(ui->entry_session_name), s->name ? s->name : "");
	gtk_editable_set_text(GTK_EDITABLE(ui->entry_dark), s->dark_path ? s->dark_path : "");
	gtk_editable_set_text(GTK_EDITABLE(ui->entry_flat), s->flat_path ? s->flat_path : "");
	gtk_editable_set_text(GTK_EDITABLE(ui->entry_bias), s->bias_path ? s->bias_path : "");
	siril_toggle_set_active(ui->check_use_existing, s->use_existing_seq);

	int count = count_files_for_subsession(ui, ui->current_subsession);
	gchar *cnt_str = g_strdup_printf(_("Assigned lights: %d frame(s)"), count);
	gtk_label_set_text(GTK_LABEL(ui->lbl_assigned_count), cnt_str);
	g_free(cnt_str);

	// Advanced settings
	gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->combo_dark_optim), s->dark_optim);
	siril_toggle_set_active(ui->check_equalize_cfa, s->equalize_cfa);
	siril_toggle_set_active(ui->check_fix_xtrans, s->fix_xtrans);
	siril_toggle_set_active(ui->check_calibrate_all, s->calibrate_all);

	// Cosmetic correction
	siril_toggle_set_active(ui->check_cosme_enable, s->cosme_enabled);
	siril_toggle_set_active(ui->check_cosme_cfa, s->cosme_cfa);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->combo_cosme_method), s->cosme_method);
	siril_toggle_set_active(ui->check_cosme_cold, s->cosme_cold);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cosme_cold), s->cosme_sig_cold > 0 ? s->cosme_sig_cold : 3.0);
	siril_toggle_set_active(ui->check_cosme_hot, s->cosme_hot);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cosme_hot), s->cosme_sig_hot > 0 ? s->cosme_sig_hot : 3.0);
	gtk_editable_set_text(GTK_EDITABLE(ui->entry_cosme_bpm), s->cosme_bpm_path ? s->cosme_bpm_path : "");
	gtk_label_set_text(GTK_LABEL(ui->lbl_cosme_eval_res), "");

	update_cosme_controls_sensitivity(ui);
}

static void update_subsessions_dropdown(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	GtkStringList *list = gtk_string_list_new(NULL);
	for (int i = 0; i < ui->num_subsessions; i++) {
		int count = count_files_for_subsession(ui, i);
		gchar *display = g_strdup_printf("%s (%d frames)", ui->subsessions[i].name, count);
		gtk_string_list_append(list, display);
		g_free(display);
	}
	g_signal_handlers_block_by_func(ui->combo_subsession, on_subsession_combo_changed, ui);
	gtk_drop_down_set_model(GTK_DROP_DOWN(ui->combo_subsession), G_LIST_MODEL(list));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->combo_subsession), ui->current_subsession);
	g_signal_handlers_unblock_by_func(ui->combo_subsession, on_subsession_combo_changed, ui);

	GtkStringList *assign_list = gtk_string_list_new(NULL);
	for (int i = 0; i < ui->num_subsessions; i++) {
		gtk_string_list_append(assign_list, ui->subsessions[i].name);
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(ui->combo_assign_target), G_LIST_MODEL(assign_list));
	g_object_unref(list);
	g_object_unref(assign_list);
}

static void on_btn_add_subsession_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	if (ui->num_subsessions >= MAX_SUBSESSIONS) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Limit Reached"),
				_("Maximum number of subsessions reached."));
		return;
	}
	save_current_subsession_fields(ui);
	int idx = ui->num_subsessions++;
	init_subsession(&ui->subsessions[idx], idx);

	ui->current_subsession = idx;
	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
}

static void on_btn_remove_subsession_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	if (ui->num_subsessions <= 1) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Cannot Remove"),
				_("At least one subsession must remain."));
		return;
	}
	int remove_idx = ui->current_subsession;
	free_subsession_fields(&ui->subsessions[remove_idx]);

	for (int i = remove_idx; i < ui->num_subsessions - 1; i++) {
		ui->subsessions[i] = ui->subsessions[i + 1];
	}
	ui->num_subsessions--;

	// Re-assign orphaned files to session 0
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->subsession_idx == remove_idx) {
			item->subsession_idx = 0;
		} else if (item->subsession_idx > remove_idx) {
			item->subsession_idx--;
		}
	}

	ui->current_subsession = (remove_idx >= ui->num_subsessions) ? ui->num_subsessions - 1 : remove_idx;
	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
	refresh_files_list_display(ui);
}

static void pick_calibration_frame(GtkEntry *entry, GtkWindow *parent, const gchar *title) {
	SirilFileBrowser *fb = siril_file_browser_new(parent, title);
	siril_file_browser_add_filter_pattern(fb, _("FITS files"),
			"*.fit;*.FIT;*.fits;*.FITS;*.fts;*.FTS;*.fit.fz;*.fits.fz", TRUE);
	const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
	if (cur && cur[0] != '\0') {
		gchar *dir = g_path_get_dirname(cur);
		siril_file_browser_set_initial_folder(fb, dir);
		g_free(dir);
	} else if (com.wd) {
		siril_file_browser_set_initial_folder(fb, com.wd);
	}

	if (siril_file_browser_run(fb) == GTK_RESPONSE_ACCEPT) {
		gchar *path = siril_file_browser_get_path(fb);
		if (path) {
			gtk_editable_set_text(GTK_EDITABLE(entry), path);
			g_free(path);
		}
	}
}

static void on_browse_dark_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	pick_calibration_frame(GTK_ENTRY(ui->entry_dark), GTK_WINDOW(ui->dialog), _("Select Master Dark"));
	save_current_subsession_fields(ui);
}

static void on_browse_flat_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	pick_calibration_frame(GTK_ENTRY(ui->entry_flat), GTK_WINDOW(ui->dialog), _("Select Master Flat"));
	save_current_subsession_fields(ui);
}

static void on_browse_bias_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	pick_calibration_frame(GTK_ENTRY(ui->entry_bias), GTK_WINDOW(ui->dialog), _("Select Master Bias / Offset"));
	save_current_subsession_fields(ui);
}

static void on_browse_bpm_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	SirilFileBrowser *fb = siril_file_browser_new(GTK_WINDOW(ui->dialog), _("Select Bad Pixel Map"));
	siril_file_browser_add_filter_pattern(fb, _("Bad Pixel Map (*.lst, *.fit, *.fits)"),
			"*.lst;*.LST;*.fit;*.FIT;*.fits;*.FITS", TRUE);
	const char *cur = gtk_editable_get_text(GTK_EDITABLE(ui->entry_cosme_bpm));
	if (cur && cur[0] != '\0') {
		gchar *dir = g_path_get_dirname(cur);
		siril_file_browser_set_initial_folder(fb, dir);
		g_free(dir);
	} else if (com.wd) {
		siril_file_browser_set_initial_folder(fb, com.wd);
	}

	if (siril_file_browser_run(fb) == GTK_RESPONSE_ACCEPT) {
		gchar *path = siril_file_browser_get_path(fb);
		if (path) {
			gtk_editable_set_text(GTK_EDITABLE(ui->entry_cosme_bpm), path);
			g_free(path);
		}
	}
	save_current_subsession_fields(ui);
}

static void on_btn_cosme_eval_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	save_current_subsession_fields(ui);
	subsession_info_t *s = &ui->subsessions[ui->current_subsession];
	if (!s->dark_path || s->dark_path[0] == '\0') {
		gtk_label_set_text(GTK_LABEL(ui->lbl_cosme_eval_res), _("No dark frame"));
		return;
	}
	fits fit = { 0 };
	fits reffit = { 0 };
	if (single_image_is_loaded())
		reffit = *gfit;
	int status;
	gchar *expression = path_parse(&reffit, s->dark_path, PATHPARSE_MODE_READ, &status);
	if (status || !expression || readfits(expression, &fit, NULL, !com.pref.force_16bit)) {
		gtk_label_set_text(GTK_LABEL(ui->lbl_cosme_eval_res), _("Failed to open dark"));
		g_free(expression);
		return;
	}
	g_free(expression);

	double sig[2];
	sig[0] = s->cosme_cold ? s->cosme_sig_cold : -1.0;
	sig[1] = s->cosme_hot ? s->cosme_sig_hot : -1.0;
	long icold = 0, ihot = 0;
	find_deviant_pixels(&fit, sig, &icold, &ihot, TRUE);
	clearfits(&fit);

	gchar *res_str = g_strdup_printf(_("%ld cold, %ld hot px"), icold, ihot);
	gtk_label_set_text(GTK_LABEL(ui->lbl_cosme_eval_res), res_str);
	g_free(res_str);
}

static void on_btn_apply_to_all_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	save_current_subsession_fields(ui);
	if (ui->current_subsession < 0 || ui->current_subsession >= ui->num_subsessions)
		return;
	subsession_info_t *src = &ui->subsessions[ui->current_subsession];
	for (int i = 0; i < ui->num_subsessions; i++) {
		if (i == ui->current_subsession) continue;
		ui->subsessions[i].dark_optim = src->dark_optim;
		ui->subsessions[i].equalize_cfa = src->equalize_cfa;
		ui->subsessions[i].fix_xtrans = src->fix_xtrans;
		ui->subsessions[i].calibrate_all = src->calibrate_all;
		ui->subsessions[i].cosme_enabled = src->cosme_enabled;
		ui->subsessions[i].cosme_cfa = src->cosme_cfa;
		ui->subsessions[i].cosme_method = src->cosme_method;
		ui->subsessions[i].cosme_cold = src->cosme_cold;
		ui->subsessions[i].cosme_sig_cold = src->cosme_sig_cold;
		ui->subsessions[i].cosme_hot = src->cosme_hot;
		ui->subsessions[i].cosme_sig_hot = src->cosme_sig_hot;
		g_free(ui->subsessions[i].cosme_bpm_path);
		ui->subsessions[i].cosme_bpm_path = g_strdup(src->cosme_bpm_path ? src->cosme_bpm_path : "");
	}
	siril_message_dialog(GTK_MESSAGE_INFO, _("Settings Applied"),
			_("Advanced calibration settings have been copied to all sessions."));
}

static void refresh_files_list_display(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	GtkWidget *child = gtk_widget_get_first_child(ui->vbox_files);
	while (child != NULL) {
		GtkWidget *next = gtk_widget_get_next_sibling(child);
		gtk_box_remove(GTK_BOX(ui->vbox_files), child);
		child = next;
	}

	int total = 0;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		total++;

		GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		item->row_box = hbox;

		GtkWidget *cb = gtk_check_button_new();
		item->check_btn = cb;
		gtk_box_append(GTK_BOX(hbox), cb);

		GtkWidget *lbl_name = gtk_label_new(item->basename);
		gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
		gtk_widget_set_hexpand(lbl_name, TRUE);
		gtk_box_append(GTK_BOX(hbox), lbl_name);

		const char *sname = (item->subsession_idx >= 0 && item->subsession_idx < ui->num_subsessions)
				? ui->subsessions[item->subsession_idx].name : _("Unassigned");
		gchar *markup = g_strdup_printf("<span background=\"#2a6f97\" foreground=\"white\" weight=\"bold\">  %s  </span>", sname);
		GtkWidget *lbl_badge = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(lbl_badge), markup);
		g_free(markup);
		item->lbl_badge = lbl_badge;
		gtk_box_append(GTK_BOX(hbox), lbl_badge);

		gtk_box_append(GTK_BOX(ui->vbox_files), hbox);
	}

	gchar *tot_str = g_strdup_printf(_("Total light frames loaded: %d"), total);
	gtk_label_set_text(GTK_LABEL(ui->lbl_total_files), tot_str);
	g_free(tot_str);
}

static void on_btn_add_lights_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	SirilFileBrowser *fb = siril_file_browser_new(GTK_WINDOW(ui->dialog), _("Add Light Frames from Sessions"));
	siril_file_browser_add_filter_pattern(fb, _("FITS files"),
			"*.fit;*.FIT;*.fits;*.FITS;*.fts;*.FTS;*.fit.fz;*.fits.fz", TRUE);
	siril_file_browser_set_select_multiple(fb, TRUE);
	if (com.wd) {
		siril_file_browser_set_initial_folder(fb, com.wd);
	}

	if (siril_file_browser_run(fb) == GTK_RESPONSE_ACCEPT) {
		GSList *paths = siril_file_browser_get_paths(fb);
		for (GSList *p = paths; p != NULL; p = p->next) {
			gchar *fpath = (gchar *)p->data;
			// Avoid duplicate paths
			gboolean exists = FALSE;
			for (GList *fl = ui->files_list; fl != NULL; fl = fl->next) {
				light_file_item_t *item = (light_file_item_t *)fl->data;
				if (g_strcmp0(item->full_path, fpath) == 0) {
					exists = TRUE;
					break;
				}
			}
			if (!exists) {
				light_file_item_t *item = calloc(1, sizeof(light_file_item_t));
				item->full_path = g_strdup(fpath);
				item->basename = g_path_get_basename(fpath);
				item->subsession_idx = ui->current_subsession;
				ui->files_list = g_list_append(ui->files_list, item);
			}
			g_free(fpath);
		}
		g_slist_free(paths);
		refresh_files_list_display(ui);
		update_subsessions_dropdown(ui);
		refresh_subsession_ui(ui);
	}
}

static void on_btn_remove_lights_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	GList *l = ui->files_list;
	while (l != NULL) {
		GList *next = l->next;
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn && siril_toggle_get_active(item->check_btn)) {
			ui->files_list = g_list_delete_link(ui->files_list, l);
			free_light_item(item);
		}
		l = next;
	}
	refresh_files_list_display(ui);
	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
}

static void on_btn_select_all_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn)
			siril_toggle_set_active(item->check_btn, TRUE);
	}
}

static void on_btn_deselect_all_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn)
			siril_toggle_set_active(item->check_btn, FALSE);
	}
}

static void on_btn_assign_selected_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	int target_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_assign_target));
	if (target_idx < 0 || target_idx >= ui->num_subsessions)
		return;

	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn && siril_toggle_get_active(item->check_btn)) {
			item->subsession_idx = target_idx;
		}
	}
	refresh_files_list_display(ui);
	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
}

static void on_btn_auto_group_by_folder_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui || ui->closing) return;
	if (!ui->files_list) return;

	GHashTable *dir_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	int assigned_subs = 0;

	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		gchar *dirname = g_path_get_dirname(item->full_path);
		gchar *base_dir = g_path_get_basename(dirname);
		g_free(dirname);

		gpointer val = g_hash_table_lookup(dir_map, base_dir);
		int sub_idx = -1;
		if (!val) {
			if (assigned_subs < ui->num_subsessions) {
				sub_idx = assigned_subs;
			} else if (ui->num_subsessions < MAX_SUBSESSIONS) {
				sub_idx = ui->num_subsessions++;
				init_subsession(&ui->subsessions[sub_idx], sub_idx);
			} else {
				sub_idx = MAX_SUBSESSIONS - 1;
			}
			g_free(ui->subsessions[sub_idx].name);
			ui->subsessions[sub_idx].name = g_strdup(base_dir);
			g_hash_table_insert(dir_map, g_strdup(base_dir), GINT_TO_POINTER(sub_idx + 1));
			assigned_subs++;
		} else {
			sub_idx = GPOINTER_TO_INT(val) - 1;
		}
		item->subsession_idx = sub_idx;
		g_free(base_dir);
	}
	g_hash_table_destroy(dir_map);

	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
	refresh_files_list_display(ui);
}

static int create_subsession_sequence_file(const char *seqname, GList *files) {
	if (!files) return 1;

	int total = g_list_length(files);
	if (total <= 0) return 1;

	// Link source files into the working directory for this subsession sequence
	GList *l = files;
	for (int i = 0; i < total; i++, l = l->next) {
		gchar *dest_file = create_sequence_filename(SEQ_REGULAR, seqname, i + 1);
		if (dest_file) {
			gchar *abs_src = g_canonicalize_filename((const gchar *)l->data, com.wd);
			gchar *abs_dest = g_build_filename(com.wd, dest_file, NULL);
			if (abs_src && abs_dest && g_strcmp0(abs_src, abs_dest) != 0) {
				symlink_uniq_file(abs_src, abs_dest, TRUE);
			}
			g_free(abs_dest);
			g_free(abs_src);
			g_free(dest_file);
		}
	}

	// Clean up any stale files from a previous larger run of this subsession
	for (int k = total + 1; ; k++) {
		gchar *extra_file = create_sequence_filename(SEQ_REGULAR, seqname, k);
		if (!extra_file) break;
		gchar *extra_dest = g_build_filename(com.wd, extra_file, NULL);
		gboolean exists = g_file_test(extra_dest, G_FILE_TEST_EXISTS);
		if (exists) {
			g_unlink(extra_dest);
			g_free(extra_dest);
			g_free(extra_file);
		} else {
			g_free(extra_dest);
			g_free(extra_file);
			break;
		}
	}

	sequence outseq;
	memset(&outseq, 0, sizeof(sequence));
	initialize_sequence(&outseq, TRUE);

	outseq.seqname = strdup(seqname);
	outseq.type = SEQ_REGULAR;
	outseq.number = total;
	outseq.fixed = 5;
	outseq.beg = 1;
	outseq.end = total;
	outseq.reference_image = 0;
	outseq.selnum = total;

	// Read geometry from first image
	const char *first_file = (const char *)files->data;
	fits first_fit = { 0 };
	int bitpix = 16, rx = 0, ry = 0, layers = 1;
	if (!read_fits_metadata_from_path(first_file, &first_fit) ||
	    !readfits(first_file, &first_fit, NULL, !com.pref.force_16bit)) {
		bitpix = first_fit.bitpix;
		rx = first_fit.rx;
		ry = first_fit.ry;
		layers = first_fit.naxes[2] > 0 ? first_fit.naxes[2] : 1;
		clearfits(&first_fit);
	}

	outseq.rx = rx;
	outseq.ry = ry;
	outseq.nb_layers = layers;
	outseq.bitpix = bitpix;

	outseq.imgparam = calloc(total, sizeof(imgdata));
	for (int i = 0; i < total; i++) {
		outseq.imgparam[i].filenum = i + 1;
		outseq.imgparam[i].incl = 1;
		outseq.imgparam[i].rx = rx;
		outseq.imgparam[i].ry = ry;
	}
	writeseqfile(&outseq);
	free_sequence(&outseq, FALSE);
	return 0;
}

struct multisession_run_data {
	GInputStream *stream;
	char *seq_to_select;
};

static gboolean multisession_select_seq_idle(gpointer user_data) {
	char *seq_to_select = (char *)user_data;
	if (seq_to_select && seq_to_select[0] != '\0') {
		gui_iface.update_sequences_list(seq_to_select);
		gui_iface.switch_to_tab(IMAGE_SEQ);
	}
	g_free(seq_to_select);
	return G_SOURCE_REMOVE;
}

static gpointer multisession_script_worker(gpointer user_data) {
	struct multisession_run_data *data = (struct multisession_run_data *)user_data;
	gpointer res = execute_script(data->stream);
	int retval = GPOINTER_TO_INT(res);
	if (!retval && data->seq_to_select) {
		g_idle_add(multisession_select_seq_idle, g_strdup(data->seq_to_select));
	}
	g_free(data->seq_to_select);
	free(data);
	return res;
}

static void on_btn_run_multisession_calib_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	save_current_subsession_fields(ui);

	if (!com.wd) {
		siril_message_dialog(GTK_MESSAGE_ERROR, _("Error"), _("No working directory is set."));
		return;
	}

	if (processing_is_job_active() || (get_script_thread_run() && !com.script_thread_exited)) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Process Running"),
				_("Another process is currently running. Please wait for it to finish."));
		return;
	}
	if (get_script_thread_run())
		wait_for_script_thread();

	// Check active subsessions
	int active_sub_count = 0;
	for (int i = 0; i < ui->num_subsessions; i++) {
		int count = count_files_for_subsession(ui, i);
		if (count > 0 || ui->subsessions[i].use_existing_seq) {
			active_sub_count++;
		}
	}

	if (active_sub_count == 0) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("No Data"),
				_("No light frames or sequences are assigned to any subsession."));
		return;
	}

	const char *outname = gtk_editable_get_text(GTK_EDITABLE(ui->entry_combined_name));
	if (!outname || outname[0] == '\0') {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Output Name"),
				_("Please specify a combined sequence name."));
		return;
	}

	gboolean debayer = siril_toggle_get_active(ui->check_debayer);
	gboolean auto_merge = siril_toggle_get_active(ui->check_auto_merge);

	GString *script = g_string_new("requires 1.2.0\n");
	GList *calibrated_seq_names = NULL;

	// Process each subsession
	for (int i = 0; i < ui->num_subsessions; i++) {
		subsession_info_t *sub = &ui->subsessions[i];
		GList *sub_files = NULL;
		for (GList *l = ui->files_list; l != NULL; l = l->next) {
			light_file_item_t *it = (light_file_item_t *)l->data;
			if (it->subsession_idx == i) {
				sub_files = g_list_append(sub_files, it->full_path);
			}
		}

		if (!sub_files && !sub->use_existing_seq)
			continue;

		gchar *seq_raw_name = NULL;
		if (sub->use_existing_seq && sub->existing_seq && sub->existing_seq[0] != '\0') {
			seq_raw_name = remove_ext_from_filename(sub->existing_seq);
		} else {
			seq_raw_name = g_strdup_printf("sess%d_raw_", i + 1);
			create_subsession_sequence_file(seq_raw_name, sub_files);
		}
		g_list_free(sub_files);

		// Calibrate command
		g_string_append_printf(script, "calibrate %s -prefix=pp_", seq_raw_name);
		if (sub->dark_path && sub->dark_path[0] != '\0') {
			gchar *trimmed = g_strstrip(g_strdup(sub->dark_path));
			if (trimmed && trimmed[0] != '\0')
				g_string_append_printf(script, " \"-dark=%s\"", trimmed);
			g_free(trimmed);
		}
		if (sub->flat_path && sub->flat_path[0] != '\0') {
			gchar *trimmed = g_strstrip(g_strdup(sub->flat_path));
			if (trimmed && trimmed[0] != '\0')
				g_string_append_printf(script, " \"-flat=%s\"", trimmed);
			g_free(trimmed);
		}
		if (sub->bias_path && sub->bias_path[0] != '\0') {
			gchar *trimmed = g_strstrip(g_strdup(sub->bias_path));
			if (trimmed && trimmed[0] != '\0')
				g_string_append_printf(script, " \"-bias=%s\"", trimmed);
			g_free(trimmed);
		}

		// Dark optimization
		if (sub->dark_path && sub->dark_path[0] != '\0') {
			if (sub->dark_optim == 1) {
				g_string_append(script, " -opt");
			} else if (sub->dark_optim == 2) {
				g_string_append(script, " -opt=exp");
			}
		}

		// Sensor & CFA corrections
		if (sub->equalize_cfa) {
			g_string_append(script, " -equalize_cfa");
		}
		if (sub->fix_xtrans) {
			g_string_append(script, " -fix_xtrans");
		}
		if (sub->calibrate_all) {
			g_string_append(script, " -all");
		}

		// Cosmetic correction
		if (sub->cosme_enabled) {
			if (sub->cosme_cfa) {
				g_string_append(script, " -cfa");
			}
			if (sub->cosme_method == 0) { // Dark-based
				if (sub->dark_path && sub->dark_path[0] != '\0') {
					double cold = sub->cosme_cold ? sub->cosme_sig_cold : 0.0;
					double hot = sub->cosme_hot ? sub->cosme_sig_hot : 0.0;
					g_string_append_printf(script, " -cc=dark %.2f %.2f", cold, hot);
				}
			} else if (sub->cosme_method == 1) { // Bad Pixel Map
				if (sub->cosme_bpm_path && sub->cosme_bpm_path[0] != '\0') {
					gchar *trimmed_bpm = g_strstrip(g_strdup(sub->cosme_bpm_path));
					if (trimmed_bpm && trimmed_bpm[0] != '\0') {
						g_string_append_printf(script, " -cc=bpm \"%s\"", trimmed_bpm);
					}
					g_free(trimmed_bpm);
				}
			}
		}

		if (debayer)
			g_string_append(script, " -debayer");
		g_string_append_c(script, '\n');

		gchar *pp_seq = g_strdup_printf("pp_%s", seq_raw_name);
		calibrated_seq_names = g_list_append(calibrated_seq_names, pp_seq);
		g_free(seq_raw_name);
	}

	gchar *trimmed_outname = g_strstrip(g_strdup(outname));
	char *seq_to_select = NULL;

	// Merge command
	if (auto_merge && g_list_length(calibrated_seq_names) >= 2) {
		g_string_append(script, "merge");
		for (GList *cl = calibrated_seq_names; cl != NULL; cl = cl->next) {
			g_string_append_printf(script, " %s", (char *)cl->data);
		}
		g_string_append_printf(script, " \"%s\"\n", trimmed_outname);
		seq_to_select = normalize_seqname(trimmed_outname, TRUE);
	} else if (g_list_length(calibrated_seq_names) == 1) {
		seq_to_select = g_strdup((char *)calibrated_seq_names->data);
	}
	g_free(trimmed_outname);
	g_list_free_full(calibrated_seq_names, g_free);

	multisession_dialog_close(ui);

	// Launch script in background
	gsize script_len = script->len;
	gchar *script_data = g_string_free(script, FALSE);
	GInputStream *input_stream = g_memory_input_stream_new_from_data(script_data, script_len, g_free);

	struct multisession_run_data *rdata = calloc(1, sizeof(struct multisession_run_data));
	rdata->stream = input_stream;
	rdata->seq_to_select = seq_to_select;

	gui_iface.show_panel("output_logs", TRUE);
	com.script_thread = g_thread_new("script", multisession_script_worker, rdata);
}

static void multisession_dialog_close(struct multisession_calib_ui *ui) {
	if (!ui || ui->closing) return;
	ui->closing = TRUE;

	if (ui->combo_subsession && GTK_IS_DROP_DOWN(ui->combo_subsession))
		g_signal_handlers_disconnect_by_data(ui->combo_subsession, ui);
	if (ui->combo_cosme_method && GTK_IS_DROP_DOWN(ui->combo_cosme_method))
		g_signal_handlers_disconnect_by_data(ui->combo_cosme_method, ui);
	if (ui->check_cosme_enable && GTK_IS_CHECK_BUTTON(ui->check_cosme_enable))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_enable, ui);
	if (ui->check_cosme_cold && GTK_IS_CHECK_BUTTON(ui->check_cosme_cold))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_cold, ui);
	if (ui->check_cosme_hot && GTK_IS_CHECK_BUTTON(ui->check_cosme_hot))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_hot, ui);

	if (ui->dialog && GTK_IS_WINDOW(ui->dialog)) {
		GtkWidget *win = ui->dialog;
		ui->dialog = NULL;
		gtk_window_destroy(GTK_WINDOW(win));
	}
}

static void on_btn_cancel_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	multisession_dialog_close(ui);
}

static void on_multisession_dialog_destroy(GtkWidget *widget, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui) return;
	ui->closing = TRUE;

	if (ui->combo_subsession && GTK_IS_DROP_DOWN(ui->combo_subsession))
		g_signal_handlers_disconnect_by_data(ui->combo_subsession, ui);
	if (ui->combo_cosme_method && GTK_IS_DROP_DOWN(ui->combo_cosme_method))
		g_signal_handlers_disconnect_by_data(ui->combo_cosme_method, ui);
	if (ui->check_cosme_enable && GTK_IS_CHECK_BUTTON(ui->check_cosme_enable))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_enable, ui);
	if (ui->check_cosme_cold && GTK_IS_CHECK_BUTTON(ui->check_cosme_cold))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_cold, ui);
	if (ui->check_cosme_hot && GTK_IS_CHECK_BUTTON(ui->check_cosme_hot))
		g_signal_handlers_disconnect_by_data(ui->check_cosme_hot, ui);

	for (int i = 0; i < ui->num_subsessions; i++) {
		free_subsession_fields(&ui->subsessions[i]);
	}
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		free_light_item((light_file_item_t *)l->data);
	}
	g_list_free(ui->files_list);
	ui->files_list = NULL;
	free(ui);
}

void show_multisession_calibration_dialog(void) {
	struct multisession_calib_ui *ui = calloc(1, sizeof(struct multisession_calib_ui));

	// Default: 2 subsessions
	ui->num_subsessions = 2;
	ui->current_subsession = 0;
	for (int i = 0; i < 2; i++) {
		init_subsession(&ui->subsessions[i], i);
	}

	GtkWidget *win = gtk_window_new();
	ui->dialog = win;
	gtk_window_set_title(GTK_WINDOW(win), _("Multi-Session Calibration & Management"));
	gtk_window_set_modal(GTK_WINDOW(win), TRUE);
	GtkWidget *control_window = lookup_widget("control_window");
	if (control_window)
		gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(control_window));
	gtk_window_set_default_size(GTK_WINDOW(win), 960, 680);

	GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(vbox_root, 15);
	gtk_widget_set_margin_end(vbox_root, 15);
	gtk_widget_set_margin_top(vbox_root, 15);
	gtk_widget_set_margin_bottom(vbox_root, 15);
	gtk_window_set_child(GTK_WINDOW(win), vbox_root);

	// Header
	GtkWidget *lbl_header = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(lbl_header),
			_("<b>Multi-Session Preprocessing</b>\nLoad lights from multiple sessions/nights at once, assign them to subsessions, and assign calibration frames and advanced correction settings to each."));
	gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
	gtk_box_append(GTK_BOX(vbox_root), lbl_header);

	// Paned split: Left = Subsession Config, Right = All Lights
	GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_vexpand(paned, TRUE);
	gtk_box_append(GTK_BOX(vbox_root), paned);

	// --- Left Pane: Subsession Config inside Scrolled Window ---
	GtkWidget *scroll_left = gtk_scrolled_window_new();
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll_left), TRUE);
	gtk_widget_set_size_request(scroll_left, 420, -1);

	GtkWidget *vbox_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(vbox_left, 8);
	gtk_widget_set_margin_end(vbox_left, 8);
	gtk_widget_set_margin_top(vbox_left, 8);
	gtk_widget_set_margin_bottom(vbox_left, 8);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_left), vbox_left);
	gtk_paned_set_start_child(GTK_PANED(paned), scroll_left);

	GtkWidget *lbl_sub_title = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(lbl_sub_title), _("<b>Subsession Configuration</b>"));
	gtk_label_set_xalign(GTK_LABEL(lbl_sub_title), 0.0);
	gtk_box_append(GTK_BOX(vbox_left), lbl_sub_title);

	// Subsession switcher row
	GtkWidget *hbox_sub_sel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	ui->combo_subsession = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_hexpand(ui->combo_subsession, TRUE);
	g_signal_connect(ui->combo_subsession, "notify::selected", G_CALLBACK(on_subsession_combo_changed), ui);
	gtk_box_append(GTK_BOX(hbox_sub_sel), ui->combo_subsession);

	GtkWidget *btn_add_sub = gtk_button_new_from_icon_name("list-add-symbolic");
	gtk_widget_set_tooltip_text(btn_add_sub, _("Add another subsession"));
	g_signal_connect(btn_add_sub, "clicked", G_CALLBACK(on_btn_add_subsession_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_sub_sel), btn_add_sub);

	GtkWidget *btn_rem_sub = gtk_button_new_from_icon_name("list-remove-symbolic");
	gtk_widget_set_tooltip_text(btn_rem_sub, _("Remove selected subsession"));
	g_signal_connect(btn_rem_sub, "clicked", G_CALLBACK(on_btn_remove_subsession_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_sub_sel), btn_rem_sub);
	gtk_box_append(GTK_BOX(vbox_left), hbox_sub_sel);

	// Subsession Name
	GtkWidget *hbox_name = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_sname = gtk_label_new(_("Name:"));
	ui->entry_session_name = gtk_entry_new();
	gtk_widget_set_hexpand(ui->entry_session_name, TRUE);
	gtk_box_append(GTK_BOX(hbox_name), lbl_sname);
	gtk_box_append(GTK_BOX(hbox_name), ui->entry_session_name);
	gtk_box_append(GTK_BOX(vbox_left), hbox_name);

	// Calibration frames group
	GtkWidget *frame_calib = gtk_frame_new(_("Calibration Frames for this Session"));
	GtkWidget *vbox_calib_inputs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(vbox_calib_inputs, 8);
	gtk_widget_set_margin_end(vbox_calib_inputs, 8);
	gtk_widget_set_margin_top(vbox_calib_inputs, 8);
	gtk_widget_set_margin_bottom(vbox_calib_inputs, 8);
	gtk_frame_set_child(GTK_FRAME(frame_calib), vbox_calib_inputs);

	// Dark
	GtkWidget *hbox_dark = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_dark = gtk_label_new(_("Master Dark:"));
	gtk_widget_set_size_request(lbl_dark, 90, -1);
	gtk_label_set_xalign(GTK_LABEL(lbl_dark), 0.0);
	ui->entry_dark = gtk_entry_new();
	gtk_widget_set_hexpand(ui->entry_dark, TRUE);
	GtkWidget *btn_dark = gtk_button_new_with_label(_("Browse..."));
	g_signal_connect(btn_dark, "clicked", G_CALLBACK(on_browse_dark_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_dark), lbl_dark);
	gtk_box_append(GTK_BOX(hbox_dark), ui->entry_dark);
	gtk_box_append(GTK_BOX(hbox_dark), btn_dark);
	gtk_box_append(GTK_BOX(vbox_calib_inputs), hbox_dark);

	// Flat
	GtkWidget *hbox_flat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_flat = gtk_label_new(_("Master Flat:"));
	gtk_widget_set_size_request(lbl_flat, 90, -1);
	gtk_label_set_xalign(GTK_LABEL(lbl_flat), 0.0);
	ui->entry_flat = gtk_entry_new();
	gtk_widget_set_hexpand(ui->entry_flat, TRUE);
	GtkWidget *btn_flat = gtk_button_new_with_label(_("Browse..."));
	g_signal_connect(btn_flat, "clicked", G_CALLBACK(on_browse_flat_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_flat), lbl_flat);
	gtk_box_append(GTK_BOX(hbox_flat), ui->entry_flat);
	gtk_box_append(GTK_BOX(hbox_flat), btn_flat);
	gtk_box_append(GTK_BOX(vbox_calib_inputs), hbox_flat);

	// Bias
	GtkWidget *hbox_bias = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_bias = gtk_label_new(_("Master Bias:"));
	gtk_widget_set_size_request(lbl_bias, 90, -1);
	gtk_label_set_xalign(GTK_LABEL(lbl_bias), 0.0);
	ui->entry_bias = gtk_entry_new();
	gtk_widget_set_hexpand(ui->entry_bias, TRUE);
	GtkWidget *btn_bias = gtk_button_new_with_label(_("Browse..."));
	g_signal_connect(btn_bias, "clicked", G_CALLBACK(on_browse_bias_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_bias), lbl_bias);
	gtk_box_append(GTK_BOX(hbox_bias), ui->entry_bias);
	gtk_box_append(GTK_BOX(hbox_bias), btn_bias);
	gtk_box_append(GTK_BOX(vbox_calib_inputs), hbox_bias);

	gtk_box_append(GTK_BOX(vbox_left), frame_calib);

	ui->check_use_existing = gtk_check_button_new_with_label(_("Use existing sequence already in folder"));
	gtk_box_append(GTK_BOX(vbox_left), ui->check_use_existing);

	ui->lbl_assigned_count = gtk_label_new(_("Assigned lights: 0 frame(s)"));
	gtk_label_set_xalign(GTK_LABEL(ui->lbl_assigned_count), 0.0);
	gtk_box_append(GTK_BOX(vbox_left), ui->lbl_assigned_count);

	// --- Advanced Calibration & Cosmetic Correction Expander ---
	GtkWidget *exp_adv = gtk_expander_new(_("Advanced Calibration Settings"));
	gtk_expander_set_expanded(GTK_EXPANDER(exp_adv), TRUE);
	GtkWidget *vbox_adv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(vbox_adv, 6);
	gtk_widget_set_margin_end(vbox_adv, 6);
	gtk_widget_set_margin_top(vbox_adv, 6);
	gtk_widget_set_margin_bottom(vbox_adv, 6);
	gtk_expander_set_child(GTK_EXPANDER(exp_adv), vbox_adv);
	gtk_box_append(GTK_BOX(vbox_left), exp_adv);

	// Dark optimization row
	GtkWidget *hbox_dark_opt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_dark_opt = gtk_label_new(_("Dark optimization:"));
	gtk_label_set_xalign(GTK_LABEL(lbl_dark_opt), 0.0);
	const char *dark_opt_strings[] = {
		_("No dark optimization"),
		_("Auto-evaluation (-opt)"),
		_("Use exposure time (-opt=exp)"),
		NULL
	};
	ui->combo_dark_optim = gtk_drop_down_new_from_strings(dark_opt_strings);
	gtk_widget_set_hexpand(ui->combo_dark_optim, TRUE);
	gtk_box_append(GTK_BOX(hbox_dark_opt), lbl_dark_opt);
	gtk_box_append(GTK_BOX(hbox_dark_opt), ui->combo_dark_optim);
	gtk_box_append(GTK_BOX(vbox_adv), hbox_dark_opt);

	// Sensor & CFA options
	ui->check_equalize_cfa = gtk_check_button_new_with_label(_("Equalize CFA layers"));
	gtk_widget_set_tooltip_text(ui->check_equalize_cfa, _("Equalizes the mean intensity of RGB layers in a CFA flat image (-equalize_cfa)"));
	gtk_box_append(GTK_BOX(vbox_adv), ui->check_equalize_cfa);

	ui->check_fix_xtrans = gtk_check_button_new_with_label(_("Fix X-Trans AF artifact"));
	gtk_widget_set_tooltip_text(ui->check_fix_xtrans, _("Fix Fujifilm X-Trans phase-detection auto-focus photosite artifact (-fix_xtrans)"));
	gtk_box_append(GTK_BOX(vbox_adv), ui->check_fix_xtrans);

	ui->check_calibrate_all = gtk_check_button_new_with_label(_("Calibrate all frames (include excluded)"));
	gtk_widget_set_tooltip_text(ui->check_calibrate_all, _("Force calibration of all frames in sequence even if marked excluded (-all)"));
	gtk_box_append(GTK_BOX(vbox_adv), ui->check_calibrate_all);

	// Cosmetic correction frame
	GtkWidget *frame_cosme = gtk_frame_new(_("Cosmetic Correction"));
	GtkWidget *vbox_cosme = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(vbox_cosme, 8);
	gtk_widget_set_margin_end(vbox_cosme, 8);
	gtk_widget_set_margin_top(vbox_cosme, 8);
	gtk_widget_set_margin_bottom(vbox_cosme, 8);
	gtk_frame_set_child(GTK_FRAME(frame_cosme), vbox_cosme);

	// Enable & CFA row
	GtkWidget *hbox_cosme_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	ui->check_cosme_enable = gtk_check_button_new_with_label(_("Enable Cosmetic Correction"));
	ui->check_cosme_cfa = gtk_check_button_new_with_label(_("CFA data (-cfa)"));
	gtk_box_append(GTK_BOX(hbox_cosme_hdr), ui->check_cosme_enable);
	gtk_box_append(GTK_BOX(hbox_cosme_hdr), ui->check_cosme_cfa);
	gtk_box_append(GTK_BOX(vbox_cosme), hbox_cosme_hdr);

	// Method selector
	GtkWidget *hbox_cmethod = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_cmethod = gtk_label_new(_("Method:"));
	const char *cmethod_strings[] = {
		_("From Master-Dark"),
		_("From Bad Pixel Map"),
		NULL
	};
	ui->combo_cosme_method = gtk_drop_down_new_from_strings(cmethod_strings);
	gtk_widget_set_hexpand(ui->combo_cosme_method, TRUE);
	gtk_box_append(GTK_BOX(hbox_cmethod), lbl_cmethod);
	gtk_box_append(GTK_BOX(hbox_cmethod), ui->combo_cosme_method);
	gtk_box_append(GTK_BOX(vbox_cosme), hbox_cmethod);

	// Master-dark CC controls
	ui->box_cosme_dark = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	GtkWidget *grid_sig = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid_sig), 6);
	gtk_grid_set_row_spacing(GTK_GRID(grid_sig), 4);

	ui->check_cosme_cold = gtk_check_button_new_with_label(_("Cold Sigma:"));
	ui->spin_cosme_cold = gtk_spin_button_new_with_range(0.1, 50.0, 0.1);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ui->spin_cosme_cold), 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cosme_cold), 3.0);
	gtk_grid_attach(GTK_GRID(grid_sig), ui->check_cosme_cold, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid_sig), ui->spin_cosme_cold, 1, 0, 1, 1);

	ui->check_cosme_hot = gtk_check_button_new_with_label(_("Hot Sigma:"));
	ui->spin_cosme_hot = gtk_spin_button_new_with_range(0.1, 50.0, 0.1);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ui->spin_cosme_hot), 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cosme_hot), 3.0);
	gtk_grid_attach(GTK_GRID(grid_sig), ui->check_cosme_hot, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid_sig), ui->spin_cosme_hot, 1, 1, 1, 1);

	gtk_box_append(GTK_BOX(ui->box_cosme_dark), grid_sig);

	// Estimate deviant pixels row
	GtkWidget *hbox_eval = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	ui->btn_cosme_eval = gtk_button_new_with_label(_("Estimate"));
	gtk_widget_set_tooltip_text(ui->btn_cosme_eval, _("Evaluate the number of cold and hot deviant pixels on the master dark"));
	g_signal_connect(ui->btn_cosme_eval, "clicked", G_CALLBACK(on_btn_cosme_eval_clicked), ui);
	ui->lbl_cosme_eval_res = gtk_label_new("");
	gtk_box_append(GTK_BOX(hbox_eval), ui->btn_cosme_eval);
	gtk_box_append(GTK_BOX(hbox_eval), ui->lbl_cosme_eval_res);
	gtk_box_append(GTK_BOX(ui->box_cosme_dark), hbox_eval);

	gtk_box_append(GTK_BOX(vbox_cosme), ui->box_cosme_dark);

	// Bad pixel map controls
	ui->box_cosme_bpm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_bpm = gtk_label_new(_("Map file:"));
	ui->entry_cosme_bpm = gtk_entry_new();
	gtk_widget_set_hexpand(ui->entry_cosme_bpm, TRUE);
	ui->btn_browse_bpm = gtk_button_new_with_label(_("Browse..."));
	g_signal_connect(ui->btn_browse_bpm, "clicked", G_CALLBACK(on_browse_bpm_clicked), ui);
	gtk_box_append(GTK_BOX(ui->box_cosme_bpm), lbl_bpm);
	gtk_box_append(GTK_BOX(ui->box_cosme_bpm), ui->entry_cosme_bpm);
	gtk_box_append(GTK_BOX(ui->box_cosme_bpm), ui->btn_browse_bpm);
	gtk_box_append(GTK_BOX(vbox_cosme), ui->box_cosme_bpm);

	gtk_box_append(GTK_BOX(vbox_adv), frame_cosme);

	// Replicate settings button
	ui->btn_apply_to_all = gtk_button_new_with_label(_("Apply Advanced Settings to All Sessions"));
	gtk_widget_set_tooltip_text(ui->btn_apply_to_all, _("Replicate the dark optimization, cosmetic correction, and sensor settings from this session to all other sessions"));
	g_signal_connect(ui->btn_apply_to_all, "clicked", G_CALLBACK(on_btn_apply_to_all_clicked), ui);
	gtk_box_append(GTK_BOX(vbox_adv), ui->btn_apply_to_all);

	// Wire sensitivity signals
	g_signal_connect_swapped(ui->check_cosme_enable, "toggled", G_CALLBACK(update_cosme_controls_sensitivity), ui);
	g_signal_connect_swapped(ui->combo_cosme_method, "notify::selected", G_CALLBACK(update_cosme_controls_sensitivity), ui);
	g_signal_connect_swapped(ui->check_cosme_cold, "toggled", G_CALLBACK(update_cosme_controls_sensitivity), ui);
	g_signal_connect_swapped(ui->check_cosme_hot, "toggled", G_CALLBACK(update_cosme_controls_sensitivity), ui);

	// --- Right Pane: All Light Frames ---
	GtkWidget *vbox_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_paned_set_end_child(GTK_PANED(paned), vbox_right);

	GtkWidget *lbl_lights_title = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(lbl_lights_title), _("<b>Light Frames (All Sessions)</b>"));
	gtk_label_set_xalign(GTK_LABEL(lbl_lights_title), 0.0);
	gtk_box_append(GTK_BOX(vbox_right), lbl_lights_title);

	// Lights toolbar
	GtkWidget *hbox_ltbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *btn_add_lights = gtk_button_new_with_label(_("Add Lights..."));
	gtk_widget_add_css_class(btn_add_lights, "suggested-action");
	g_signal_connect(btn_add_lights, "clicked", G_CALLBACK(on_btn_add_lights_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_ltbar), btn_add_lights);

	GtkWidget *btn_rem_lights = gtk_button_new_with_label(_("Remove"));
	g_signal_connect(btn_rem_lights, "clicked", G_CALLBACK(on_btn_remove_lights_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_ltbar), btn_rem_lights);

	GtkWidget *btn_autogroup = gtk_button_new_with_label(_("Auto-Group by Folder"));
	gtk_widget_set_tooltip_text(btn_autogroup, _("Automatically group files into subsessions based on their parent directory names"));
	g_signal_connect(btn_autogroup, "clicked", G_CALLBACK(on_btn_auto_group_by_folder_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_ltbar), btn_autogroup);

	gtk_box_append(GTK_BOX(vbox_right), hbox_ltbar);

	// Assignment row
	GtkWidget *hbox_assign = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl_assign = gtk_label_new(_("Assign checked to:"));
	ui->combo_assign_target = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_hexpand(ui->combo_assign_target, TRUE);
	GtkWidget *btn_assign = gtk_button_new_with_label(_("Assign"));
	g_signal_connect(btn_assign, "clicked", G_CALLBACK(on_btn_assign_selected_clicked), ui);
	GtkWidget *btn_sel_all = gtk_button_new_with_label(_("All"));
	g_signal_connect(btn_sel_all, "clicked", G_CALLBACK(on_btn_select_all_clicked), ui);
	GtkWidget *btn_desel_all = gtk_button_new_with_label(_("None"));
	g_signal_connect(btn_desel_all, "clicked", G_CALLBACK(on_btn_deselect_all_clicked), ui);

	gtk_box_append(GTK_BOX(hbox_assign), lbl_assign);
	gtk_box_append(GTK_BOX(hbox_assign), ui->combo_assign_target);
	gtk_box_append(GTK_BOX(hbox_assign), btn_assign);
	gtk_box_append(GTK_BOX(hbox_assign), btn_sel_all);
	gtk_box_append(GTK_BOX(hbox_assign), btn_desel_all);
	gtk_box_append(GTK_BOX(vbox_right), hbox_assign);

	// Scrolled window for files list
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_widget_set_vexpand(scroll, TRUE);

	ui->vbox_files = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(ui->vbox_files, 8);
	gtk_widget_set_margin_end(ui->vbox_files, 8);
	gtk_widget_set_margin_top(ui->vbox_files, 8);
	gtk_widget_set_margin_bottom(ui->vbox_files, 8);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui->vbox_files);
	gtk_box_append(GTK_BOX(vbox_right), scroll);

	ui->lbl_total_files = gtk_label_new(_("Total light frames loaded: 0"));
	gtk_label_set_xalign(GTK_LABEL(ui->lbl_total_files), 0.0);
	gtk_box_append(GTK_BOX(vbox_right), ui->lbl_total_files);

	// --- Bottom Bar: Global Options & Actions ---
	GtkWidget *hbox_bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top(hbox_bottom, 6);

	ui->check_debayer = gtk_check_button_new_with_label(_("Debayer CFA"));
	siril_toggle_set_active(ui->check_debayer, TRUE);
	gtk_box_append(GTK_BOX(hbox_bottom), ui->check_debayer);

	ui->check_auto_merge = gtk_check_button_new_with_label(_("Combine calibrated sessions into unified sequence"));
	siril_toggle_set_active(ui->check_auto_merge, TRUE);
	gtk_box_append(GTK_BOX(hbox_bottom), ui->check_auto_merge);

	GtkWidget *lbl_comb = gtk_label_new(_("Combined sequence:"));
	ui->entry_combined_name = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(ui->entry_combined_name), "multisession_pp_lights_");
	gtk_box_append(GTK_BOX(hbox_bottom), lbl_comb);
	gtk_box_append(GTK_BOX(hbox_bottom), ui->entry_combined_name);

	GtkWidget *hbox_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(hbox_actions, GTK_ALIGN_END);
	gtk_widget_set_hexpand(hbox_actions, TRUE);

	GtkWidget *btn_cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_actions), btn_cancel);

	GtkWidget *btn_run = gtk_button_new_with_label(_("Calibrate All Sessions"));
	gtk_widget_add_css_class(btn_run, "suggested-action");
	g_signal_connect(btn_run, "clicked", G_CALLBACK(on_btn_run_multisession_calib_clicked), ui);
	gtk_box_append(GTK_BOX(hbox_actions), btn_run);

	gtk_box_append(GTK_BOX(hbox_bottom), hbox_actions);
	gtk_box_append(GTK_BOX(vbox_root), hbox_bottom);

	g_signal_connect(win, "destroy", G_CALLBACK(on_multisession_dialog_destroy), ui);

	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
	gtk_window_present(GTK_WINDOW(win));
}

void on_btn_multisession_calib_clicked(GtkButton *button, gpointer user_data) {
	show_multisession_calibration_dialog();
}
