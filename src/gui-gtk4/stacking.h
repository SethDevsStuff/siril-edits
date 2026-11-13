#ifndef _GUI_STACKING_H_
#define _GUI_STACKING_H_

#include "core/sequence_filtering.h"

void initialize_stacking_methods();
void update_stack_interface(gboolean dont_change_stack_type);
void get_sequence_filtering_from_gui(seq_image_filter *filtering_criterion, double *filtering_parameter);
void on_btn_multisession_clicked(GtkButton *button, gpointer user_data);
void on_check_split_dual_narrowband_toggled(GtkCheckButton *button, gpointer user_data);

#endif
