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
#include "core/proto.h"
#include "core/siril.h"
#include "core/siril_log.h"
#include "gui-gtk4/file_browser.h"
#include "gui-gtk4/message_dialog.h"
#include "gui-gtk4/multisession.h"
#include "gui-gtk4/utils.h"
#include "io/conversion.h"
#include "io/image_format_fits.h"
#include "io/sequence.h"

#define MAX_SUBSESSIONS 16

typedef struct {
	gchar *name;
	gchar *dark_path;
	gchar *flat_path;
	gchar *bias_path;
	gchar *existing_seq;
	gboolean use_existing_seq;
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

static void free_light_item(light_file_item_t *item) {
	if (!item) return;
	g_free(item->full_path);
	g_free(item->basename);
	free(item);
}

static void on_subsession_combo_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	int sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_subsession));
	if (sel >= 0 && sel < ui->num_subsessions && sel != ui->current_subsession) {
		save_current_subsession_fields(ui);
		ui->current_subsession = sel;
		refresh_subsession_ui(ui);
	}
}

static void save_current_subsession_fields(struct multisession_calib_ui *ui) {
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
}

static int count_files_for_subsession(struct multisession_calib_ui *ui, int sub_idx) {
	int count = 0;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->subsession_idx == sub_idx) count++;
	}
	return count;
}

static void refresh_subsession_ui(struct multisession_calib_ui *ui) {
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
}

static void update_subsessions_dropdown(struct multisession_calib_ui *ui) {
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
	if (ui->num_subsessions >= MAX_SUBSESSIONS) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Limit Reached"),
				_("Maximum number of subsessions reached."));
		return;
	}
	save_current_subsession_fields(ui);
	int idx = ui->num_subsessions++;
	ui->subsessions[idx].name = g_strdup_printf("Session %d", idx + 1);
	ui->subsessions[idx].dark_path = g_strdup("");
	ui->subsessions[idx].flat_path = g_strdup("");
	ui->subsessions[idx].bias_path = g_strdup("");
	ui->subsessions[idx].existing_seq = g_strdup("");
	ui->subsessions[idx].use_existing_seq = FALSE;

	ui->current_subsession = idx;
	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
}

static void on_btn_remove_subsession_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (ui->num_subsessions <= 1) {
		siril_message_dialog(GTK_MESSAGE_WARNING, _("Cannot Remove"),
				_("At least one subsession must remain."));
		return;
	}
	int remove_idx = ui->current_subsession;
	g_free(ui->subsessions[remove_idx].name);
	g_free(ui->subsessions[remove_idx].dark_path);
	g_free(ui->subsessions[remove_idx].flat_path);
	g_free(ui->subsessions[remove_idx].bias_path);
	g_free(ui->subsessions[remove_idx].existing_seq);

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
	pick_calibration_frame(GTK_ENTRY(ui->entry_dark), GTK_WINDOW(ui->dialog), _("Select Master Dark"));
	save_current_subsession_fields(ui);
}

static void on_browse_flat_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	pick_calibration_frame(GTK_ENTRY(ui->entry_flat), GTK_WINDOW(ui->dialog), _("Select Master Flat"));
	save_current_subsession_fields(ui);
}

static void on_browse_bias_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	pick_calibration_frame(GTK_ENTRY(ui->entry_bias), GTK_WINDOW(ui->dialog), _("Select Master Bias / Offset"));
	save_current_subsession_fields(ui);
}

static void refresh_files_list_display(struct multisession_calib_ui *ui) {
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
		gtk_widget_set_tooltip_text(lbl_name, item->full_path);
		gtk_box_append(GTK_BOX(hbox), lbl_name);

		const char *sname = (item->subsession_idx < ui->num_subsessions) ?
				ui->subsessions[item->subsession_idx].name : _("Unassigned");
		GtkWidget *badge = gtk_label_new(sname);
		item->lbl_badge = badge;
		gtk_widget_add_css_class(badge, "badge");
		gtk_box_append(GTK_BOX(hbox), badge);

		gtk_box_append(GTK_BOX(ui->vbox_files), hbox);
	}

	gchar *tot_str = g_strdup_printf(_("Total light frames loaded: %d"), total);
	gtk_label_set_text(GTK_LABEL(ui->lbl_total_files), tot_str);
	g_free(tot_str);

	update_subsessions_dropdown(ui);
	refresh_subsession_ui(ui);
}

static void on_btn_add_lights_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	SirilFileBrowser *fb = siril_file_browser_new(GTK_WINDOW(ui->dialog), _("Add Multi-Session Light Frames"));
	siril_file_browser_set_select_multiple(fb, TRUE);
	siril_file_browser_add_filter_pattern(fb, _("Supported Images"),
			"*.fit;*.FIT;*.fits;*.FITS;*.fts;*.FTS;*.fit.fz;*.fits.fz;*.cr2;*.CR2;*.cr3;*.CR3;*.nef;*.NEF;*.arw;*.ARW;*.dng;*.DNG;*.tif;*.tiff;*.TIF;*.TIFF", TRUE);
	if (com.wd) siril_file_browser_set_initial_folder(fb, com.wd);

	if (siril_file_browser_run(fb) == GTK_RESPONSE_ACCEPT) {
		GSList *paths = siril_file_browser_get_paths(fb);
		for (GSList *p = paths; p != NULL; p = p->next) {
			const char *fpath = (const char *)p->data;
			if (!fpath) continue;

			// Check duplicate
			gboolean exists = FALSE;
			for (GList *fl = ui->files_list; fl != NULL; fl = fl->next) {
				light_file_item_t *it = (light_file_item_t *)fl->data;
				if (!g_strcmp0(it->full_path, fpath)) {
					exists = TRUE;
					break;
				}
			}
			if (exists) continue;

			light_file_item_t *item = calloc(1, sizeof(light_file_item_t));
			item->full_path = g_strdup(fpath);
			item->basename = g_path_get_basename(fpath);
			item->subsession_idx = ui->current_subsession;
			ui->files_list = g_list_append(ui->files_list, item);
		}
		g_slist_free_full(paths, g_free);
		refresh_files_list_display(ui);
	}
}

static void on_btn_remove_lights_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
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
}

static void on_btn_select_all_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn) siril_toggle_set_active(item->check_btn, TRUE);
	}
}

static void on_btn_deselect_all_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn) siril_toggle_set_active(item->check_btn, FALSE);
	}
}

static void on_btn_assign_selected_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	int target = gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->combo_assign_target));
	if (target < 0 || target >= ui->num_subsessions) return;

	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		if (item->check_btn && siril_toggle_get_active(item->check_btn)) {
			item->subsession_idx = target;
		}
	}
	refresh_files_list_display(ui);
}

static void on_btn_auto_group_by_folder_clicked(GtkButton *btn, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	if (!ui->files_list) return;

	GList *unique_dirs = NULL;
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		light_file_item_t *item = (light_file_item_t *)l->data;
		gchar *dir = g_path_get_dirname(item->full_path);
		gboolean found = FALSE;
		for (GList *dl = unique_dirs; dl != NULL; dl = dl->next) {
			if (!g_strcmp0((char *)dl->data, dir)) {
				found = TRUE;
				break;
			}
		}
		if (!found) {
			unique_dirs = g_list_append(unique_dirs, dir);
		} else {
			g_free(dir);
		}
	}

	int dir_count = g_list_length(unique_dirs);
	if (dir_count <= 1) {
		g_list_free_full(unique_dirs, g_free);
		siril_message_dialog(GTK_MESSAGE_INFO, _("Auto-Group by Folder"),
				_("All loaded files originate from the same directory.\nPlease use manual assignment if they belong to different subsessions."));
		return;
	}

	save_current_subsession_fields(ui);

	// Ensure we have enough subsessions
	while (ui->num_subsessions < dir_count && ui->num_subsessions < MAX_SUBSESSIONS) {
		int idx = ui->num_subsessions++;
		ui->subsessions[idx].name = g_strdup_printf("Session %d", idx + 1);
		ui->subsessions[idx].dark_path = g_strdup("");
		ui->subsessions[idx].flat_path = g_strdup("");
		ui->subsessions[idx].bias_path = g_strdup("");
		ui->subsessions[idx].existing_seq = g_strdup("");
		ui->subsessions[idx].use_existing_seq = FALSE;
	}

	// Rename subsessions to folder basenames and assign files
	int d_idx = 0;
	for (GList *dl = unique_dirs; dl != NULL && d_idx < ui->num_subsessions; dl = dl->next, d_idx++) {
		const char *dpath = (const char *)dl->data;
		gchar *bname = g_path_get_basename(dpath);
		g_free(ui->subsessions[d_idx].name);
		ui->subsessions[d_idx].name = g_strdup_printf("Session %d (%s)", d_idx + 1, bname);
		g_free(bname);

		for (GList *fl = ui->files_list; fl != NULL; fl = fl->next) {
			light_file_item_t *item = (light_file_item_t *)fl->data;
			gchar *fdir = g_path_get_dirname(item->full_path);
			if (!g_strcmp0(fdir, dpath)) {
				item->subsession_idx = d_idx;
			}
			g_free(fdir);
		}
	}
	g_list_free_full(unique_dirs, g_free);

	ui->current_subsession = 0;
	refresh_files_list_display(ui);
	siril_message_dialog(GTK_MESSAGE_INFO, _("Auto-Group Completed"),
			_("Light frames have been automatically grouped into subsessions by folder."));
}

static int create_subsession_sequence_file(const char *seqname, GList *file_paths) {
	int count = g_list_length(file_paths);
	if (count == 0) return -1;

	int rx = 0, ry = 0, nb_layers = 1, bitpix = 16;

	// Create symlinks/copies/converted FITS in com.wd
	for (int i = 0; i < count; i++) {
		const char *src = (const char *)g_list_nth_data(file_paths, i);
		gchar *dest_file = create_sequence_filename(SEQ_REGULAR, seqname, i + 1);
		gchar *dest_path = g_build_filename(com.wd, dest_file, NULL);

		if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
			g_unlink(dest_path);
		}

		const char *ext = get_filename_ext(src);
		image_type itype = ext ? get_type_for_extension(ext) : TYPEUNDEF;

		if (itype == TYPEFITS) {
			if (symlink(src, dest_path) != 0) {
				// Fallback: copy file
				GFile *src_f = g_file_new_for_path(src);
				GFile *dst_f = g_file_new_for_path(dest_path);
				g_file_copy(src_f, dst_f, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
				g_object_unref(src_f);
				g_object_unref(dst_f);
			}
			if (rx == 0) {
				fits first_fit = { 0 };
				if (readfits(dest_path, &first_fit, NULL, FALSE) == 0) {
					rx = first_fit.rx;
					ry = first_fit.ry;
					nb_layers = (first_fit.naxes[2] > 0) ? first_fit.naxes[2] : 1;
					bitpix = first_fit.bitpix ? first_fit.bitpix : 16;
					clearfits(&first_fit);
				}
			}
		} else {
			fits cur_fit = { 0 };
			if (any_to_fits(itype, src, &cur_fit, FALSE, FALSE) == 0) {
				if (rx == 0) {
					rx = cur_fit.rx;
					ry = cur_fit.ry;
					nb_layers = (cur_fit.naxes[2] > 0) ? cur_fit.naxes[2] : 1;
					bitpix = cur_fit.bitpix ? cur_fit.bitpix : 16;
				}
				savefits(dest_path, &cur_fit);
				clearfits(&cur_fit);
			}
		}
		g_free(dest_file);
		g_free(dest_path);
	}

	sequence outseq;
	memset(&outseq, 0, sizeof(sequence));
	initialize_sequence(&outseq, TRUE);
	outseq.seqname = remove_ext_from_filename(seqname);
	outseq.type = SEQ_REGULAR;
	outseq.number = count;
	outseq.selnum = count;
	outseq.fixed = 5;
	outseq.beg = 1;
	outseq.end = count;
	outseq.rx = rx;
	outseq.ry = ry;
	outseq.nb_layers = nb_layers;
	outseq.bitpix = bitpix;
	outseq.reference_image = 0;

	outseq.imgparam = calloc(count, sizeof(imgdata));
	for (int i = 0; i < count; i++) {
		outseq.imgparam[i].filenum = i + 1;
		outseq.imgparam[i].incl = 1;
		outseq.imgparam[i].rx = rx;
		outseq.imgparam[i].ry = ry;
	}
	writeseqfile(&outseq);
	free_sequence(&outseq, FALSE);
	return 0;
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
		if (debayer)
			g_string_append(script, " -debayer");
		g_string_append_c(script, '\n');

		gchar *pp_seq = g_strdup_printf("pp_%s", seq_raw_name);
		calibrated_seq_names = g_list_append(calibrated_seq_names, pp_seq);
		g_free(seq_raw_name);
	}

	// Merge command
	if (auto_merge && g_list_length(calibrated_seq_names) >= 2) {
		g_string_append(script, "merge");
		for (GList *cl = calibrated_seq_names; cl != NULL; cl = cl->next) {
			g_string_append_printf(script, " %s", (char *)cl->data);
		}
		g_string_append_printf(script, " \"%s\"\n", outname);
	}
	g_list_free_full(calibrated_seq_names, g_free);

	gtk_window_destroy(GTK_WINDOW(ui->dialog));

	// Launch script in background
	gsize script_len = script->len;
	gchar *script_data = g_string_free(script, FALSE);
	GInputStream *input_stream = g_memory_input_stream_new_from_data(script_data, script_len, g_free);

	gui_iface.show_panel("output_logs", TRUE);
	com.script_thread = g_thread_new("script", execute_script, input_stream);
}

static void on_multisession_dialog_destroy(GtkWidget *widget, gpointer user_data) {
	struct multisession_calib_ui *ui = (struct multisession_calib_ui *)user_data;
	for (int i = 0; i < ui->num_subsessions; i++) {
		g_free(ui->subsessions[i].name);
		g_free(ui->subsessions[i].dark_path);
		g_free(ui->subsessions[i].flat_path);
		g_free(ui->subsessions[i].bias_path);
		g_free(ui->subsessions[i].existing_seq);
	}
	for (GList *l = ui->files_list; l != NULL; l = l->next) {
		free_light_item((light_file_item_t *)l->data);
	}
	g_list_free(ui->files_list);
	free(ui);
}

void show_multisession_calibration_dialog(void) {
	struct multisession_calib_ui *ui = calloc(1, sizeof(struct multisession_calib_ui));

	// Default: 2 subsessions
	ui->num_subsessions = 2;
	ui->current_subsession = 0;
	for (int i = 0; i < 2; i++) {
		ui->subsessions[i].name = g_strdup_printf("Session %d", i + 1);
		ui->subsessions[i].dark_path = g_strdup("");
		ui->subsessions[i].flat_path = g_strdup("");
		ui->subsessions[i].bias_path = g_strdup("");
		ui->subsessions[i].existing_seq = g_strdup("");
	}

	GtkWidget *win = gtk_window_new();
	ui->dialog = win;
	gtk_window_set_title(GTK_WINDOW(win), _("Multi-Session Calibration & Management"));
	gtk_window_set_modal(GTK_WINDOW(win), TRUE);
	GtkWidget *control_window = lookup_widget("control_window");
	if (control_window)
		gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(control_window));
	gtk_window_set_default_size(GTK_WINDOW(win), 860, 580);

	GtkWidget *vbox_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_start(vbox_root, 15);
	gtk_widget_set_margin_end(vbox_root, 15);
	gtk_widget_set_margin_top(vbox_root, 15);
	gtk_widget_set_margin_bottom(vbox_root, 15);
	gtk_window_set_child(GTK_WINDOW(win), vbox_root);

	// Header
	GtkWidget *lbl_header = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(lbl_header),
			_("<b>Multi-Session Preprocessing</b>\nLoad lights from multiple sessions/nights at once, assign them to subsessions, and assign calibration frames to each."));
	gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0);
	gtk_box_append(GTK_BOX(vbox_root), lbl_header);

	// Paned split: Left = Subsession Config, Right = All Lights
	GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_vexpand(paned, TRUE);
	gtk_box_append(GTK_BOX(vbox_root), paned);

	// --- Left Pane: Subsession Config ---
	GtkWidget *vbox_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_size_request(vbox_left, 380, -1);
	gtk_paned_set_start_child(GTK_PANED(paned), vbox_left);

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
	g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
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
