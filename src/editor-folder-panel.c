/* editor-folder-panel.c
 *
 * Copyright 2026 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "editor-folder-panel"

#include "config.h"

#include <glib/gi18n.h>
#include <adwaita.h>

#include "editor-application.h"
#include "editor-folder-panel.h"
#include "editor-session-private.h"
#include "editor-window-private.h"

struct _EditorFolderPanel
{
  GtkWidget parent_instance;

  /* Template references */
  GtkBox            *header_box;
  GtkImage          *header_icon;
  GtkLabel          *folder_name_label;
  GtkButton         *open_folder_button;
  GtkSearchEntry    *search_entry;
  GtkStack          *stack;
  AdwStatusPage     *empty_page;
  GtkScrolledWindow *scrolled_window;
  GtkListView       *list_view;

  /* Data */
  GFile             *folder;
  GtkTreeListModel  *tree_model;
  GtkCustomSorter   *sorter;
  GtkCustomFilter   *filter;
  char              *filter_text;
};

enum {
  PROP_0,
  PROP_FOLDER,
  N_PROPS
};

enum {
  OPEN_FILE,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (EditorFolderPanel, editor_folder_panel, GTK_TYPE_WIDGET)

static GFile *
get_file_for_tree_row (EditorFolderPanel *self,
                       GtkTreeListRow    *row)
{
  g_autoptr(GFileInfo) info = NULL;
  GFile *file;
  g_autoptr(GtkTreeListRow) parent_row = NULL;

  if (row == NULL)
    return NULL;

  info = gtk_tree_list_row_get_item (row);
  if (info == NULL)
    return NULL;

  file = G_FILE (g_file_info_get_attribute_object (info, "standard::file"));
  if (file != NULL)
    return g_object_ref (file);

  parent_row = gtk_tree_list_row_get_parent (row);
  if (parent_row != NULL)
    {
      g_autoptr(GFile) parent_file = get_file_for_tree_row (self, parent_row);
      if (parent_file != NULL)
        return g_file_get_child (parent_file, g_file_info_get_name (info));
    }
  else if (self->folder != NULL)
    {
      return g_file_get_child (self->folder, g_file_info_get_name (info));
    }

  return NULL;
}

static int
compare_file_info (gconstpointer a,
                   gconstpointer b,
                   gpointer      user_data)
{
  GFileInfo *info_a = (GFileInfo *)a;
  GFileInfo *info_b = (GFileInfo *)b;
  gboolean is_dir_a, is_dir_b;
  const char *name_a, *name_b;

  if (info_a == info_b)
    return 0;
  if (info_a == NULL)
    return 1;
  if (info_b == NULL)
    return -1;

  is_dir_a = (g_file_info_get_file_type (info_a) == G_FILE_TYPE_DIRECTORY);
  is_dir_b = (g_file_info_get_file_type (info_b) == G_FILE_TYPE_DIRECTORY);

  if (is_dir_a != is_dir_b)
    return is_dir_a ? -1 : 1;

  name_a = g_file_info_get_display_name (info_a);
  if (name_a == NULL)
    name_a = g_file_info_get_name (info_a);

  name_b = g_file_info_get_display_name (info_b);
  if (name_b == NULL)
    name_b = g_file_info_get_name (info_b);

  if (name_a == NULL && name_b == NULL)
    return 0;
  if (name_a == NULL)
    return 1;
  if (name_b == NULL)
    return -1;

  return g_utf8_collate (name_a, name_b);
}

static gboolean
match_file_info (gpointer item,
                 gpointer user_data)
{
  GFileInfo *info = G_FILE_INFO (item);
  EditorFolderPanel *self = user_data;
  const char *name;

  if (info == NULL)
    return FALSE;

  name = g_file_info_get_name (info);
  if (name == NULL)
    return FALSE;

  /* Always ignore .git directory to avoid recursion and clutter */
  if (g_strcmp0 (name, ".git") == 0)
    return FALSE;

  if (self->filter_text != NULL && self->filter_text[0] != '\0')
    {
      const char *display_name = g_file_info_get_display_name (info);
      g_autofree char *casefold = NULL;

      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        return TRUE;

      if (display_name == NULL)
        display_name = name;

      casefold = g_utf8_casefold (display_name, -1);
      return strstr (casefold, self->filter_text) != NULL;
    }

  return TRUE;
}

static GListModel *
create_folder_model (EditorFolderPanel *self,
                     GFile             *dir)
{
  GtkDirectoryList *dir_list;
  GtkFilterListModel *filter_model;
  GtkSortListModel *sort_model;

  g_assert (EDITOR_IS_FOLDER_PANEL (self));
  g_assert (G_IS_FILE (dir));

  dir_list = gtk_directory_list_new ("standard::name,standard::display-name,standard::symbolic-icon,standard::type,standard::is-hidden,standard::is-backup,standard::file", dir);
  gtk_directory_list_set_monitored (dir_list, TRUE);

  filter_model = gtk_filter_list_model_new (G_LIST_MODEL (dir_list),
                                            g_object_ref (GTK_FILTER (self->filter)));

  sort_model = gtk_sort_list_model_new (G_LIST_MODEL (filter_model),
                                        g_object_ref (GTK_SORTER (self->sorter)));

  return G_LIST_MODEL (sort_model);
}

static GListModel *
create_tree_model_func (gpointer item,
                        gpointer user_data)
{
  EditorFolderPanel *self = user_data;
  GFileInfo *info = G_FILE_INFO (item);
  GFile *file;

  if (info == NULL)
    return NULL;

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    return NULL;

  file = G_FILE (g_file_info_get_attribute_object (info, "standard::file"));
  if (file == NULL)
    return NULL;

  return create_folder_model (self, file);
}

static void
on_list_view_activate_cb (EditorFolderPanel *self,
                          guint              position,
                          GtkListView       *list_view)
{
  GtkSelectionModel *selection_model;
  g_autoptr(GtkTreeListRow) row = NULL;
  g_autoptr(GFileInfo) info = NULL;

  g_assert (EDITOR_IS_FOLDER_PANEL (self));
  g_assert (GTK_IS_LIST_VIEW (list_view));

  selection_model = gtk_list_view_get_model (list_view);
  if (selection_model == NULL)
    return;

  row = g_list_model_get_item (G_LIST_MODEL (selection_model), position);
  if (row == NULL)
    return;

  info = gtk_tree_list_row_get_item (row);
  if (info == NULL)
    return;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      gboolean expanded = gtk_tree_list_row_get_expanded (row);
      gtk_tree_list_row_set_expanded (row, !expanded);
    }
  else
    {
      g_autoptr(GFile) file = get_file_for_tree_row (self, row);
      if (file != NULL)
        {
          GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

          g_signal_emit (self, signals[OPEN_FILE], 0, file);

          if (EDITOR_IS_WINDOW (root))
            {
              EditorSession *session = editor_application_get_session (EDITOR_APPLICATION_DEFAULT);
              editor_session_open (session, EDITOR_WINDOW (root), file, NULL);
            }
        }
    }
}

static void
on_factory_setup_cb (GtkSignalListItemFactory *factory,
                     GtkListItem              *list_item,
                     gpointer                  user_data)
{
  GtkWidget *expander = gtk_tree_expander_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon = gtk_image_new ();
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_set_margin_top (box, 2);
  gtk_widget_set_margin_bottom (box, 2);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);

  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);

  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), box);

  gtk_list_item_set_child (list_item, expander);
}

static void
on_factory_bind_cb (GtkSignalListItemFactory *factory,
                    GtkListItem              *list_item,
                    gpointer                  user_data)
{
  GtkWidget *expander;
  GtkWidget *box;
  GtkWidget *icon;
  GtkWidget *label;
  GtkTreeListRow *row;
  g_autoptr(GFileInfo) info = NULL;

  expander = gtk_list_item_get_child (list_item);
  box = gtk_tree_expander_get_child (GTK_TREE_EXPANDER (expander));
  icon = gtk_widget_get_first_child (box);
  label = gtk_widget_get_next_sibling (icon);

  row = gtk_list_item_get_item (list_item);
  if (row == NULL)
    return;

  gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

  info = gtk_tree_list_row_get_item (row);
  if (info != NULL)
    {
      const char *display_name = g_file_info_get_display_name (info);
      GIcon *gicon;

      if (display_name == NULL)
        display_name = g_file_info_get_name (info);

      gtk_label_set_text (GTK_LABEL (label), display_name ? display_name : "");

      gicon = g_file_info_get_symbolic_icon (info);
      if (gicon != NULL)
        gtk_image_set_from_gicon (GTK_IMAGE (icon), gicon);
      else if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        gtk_image_set_from_icon_name (GTK_IMAGE (icon), "folder-symbolic");
      else
        gtk_image_set_from_icon_name (GTK_IMAGE (icon), "text-x-generic-symbolic");

      if (g_file_info_get_is_hidden (info))
        gtk_widget_add_css_class (label, "dim-label");
      else
        gtk_widget_remove_css_class (label, "dim-label");
    }
}

static void
on_factory_unbind_cb (GtkSignalListItemFactory *factory,
                      GtkListItem              *list_item,
                      gpointer                  user_data)
{
  GtkWidget *expander = gtk_list_item_get_child (list_item);
  if (expander != NULL)
    gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);
}

static void
on_factory_teardown_cb (GtkSignalListItemFactory *factory,
                        GtkListItem              *list_item,
                        gpointer                  user_data)
{
  gtk_list_item_set_child (list_item, NULL);
}

static void
on_search_entry_changed_cb (EditorFolderPanel *self,
                            GtkSearchEntry    *search_entry)
{
  const char *text;

  g_assert (EDITOR_IS_FOLDER_PANEL (self));
  g_assert (GTK_IS_SEARCH_ENTRY (search_entry));

  text = gtk_editable_get_text (GTK_EDITABLE (search_entry));

  g_free (self->filter_text);
  if (text != NULL && text[0] != '\0')
    self->filter_text = g_utf8_casefold (text, -1);
  else
    self->filter_text = NULL;

  if (self->filter != NULL)
    gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_search_entry_stop_search_cb (EditorFolderPanel *self,
                                GtkSearchEntry    *search_entry)
{
  g_assert (EDITOR_IS_FOLDER_PANEL (self));
  g_assert (GTK_IS_SEARCH_ENTRY (search_entry));

  gtk_editable_set_text (GTK_EDITABLE (search_entry), "");
}

static void
editor_folder_panel_dispose (GObject *object)
{
  EditorFolderPanel *self = (EditorFolderPanel *)object;
  GtkWidget *child;

  if (self->list_view != NULL)
    gtk_list_view_set_model (self->list_view, NULL);

  g_clear_object (&self->folder);
  g_clear_object (&self->tree_model);
  g_clear_object (&self->sorter);
  g_clear_object (&self->filter);
  g_clear_pointer (&self->filter_text, g_free);

  gtk_widget_dispose_template (GTK_WIDGET (self), EDITOR_TYPE_FOLDER_PANEL);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (editor_folder_panel_parent_class)->dispose (object);
}

static void
editor_folder_panel_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  EditorFolderPanel *self = EDITOR_FOLDER_PANEL (object);

  switch (prop_id)
    {
    case PROP_FOLDER:
      g_value_set_object (value, editor_folder_panel_get_folder (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_folder_panel_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  EditorFolderPanel *self = EDITOR_FOLDER_PANEL (object);

  switch (prop_id)
    {
    case PROP_FOLDER:
      editor_folder_panel_set_folder (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_folder_panel_class_init (EditorFolderPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = editor_folder_panel_dispose;
  object_class->get_property = editor_folder_panel_get_property;
  object_class->set_property = editor_folder_panel_set_property;

  properties[PROP_FOLDER] =
    g_param_spec_object ("folder",
                         NULL,
                         NULL,
                         G_TYPE_FILE,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[OPEN_FILE] =
    g_signal_new ("open-file",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_FILE);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/TextEditor/ui/editor-folder-panel.ui");

  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, header_box);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, header_icon);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, folder_name_label);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, open_folder_button);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, search_entry);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, stack);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, empty_page);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, EditorFolderPanel, list_view);

  gtk_widget_class_bind_template_callback (widget_class, on_search_entry_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_search_entry_stop_search_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_list_view_activate_cb);
}

static void
editor_folder_panel_init (EditorFolderPanel *self)
{
  GtkListItemFactory *factory;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->sorter = gtk_custom_sorter_new (compare_file_info, self, NULL);
  self->filter = gtk_custom_filter_new (match_file_info, self, NULL);

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_factory_setup_cb), self);
  g_signal_connect (factory, "bind", G_CALLBACK (on_factory_bind_cb), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (on_factory_unbind_cb), self);
  g_signal_connect (factory, "teardown", G_CALLBACK (on_factory_teardown_cb), self);
  gtk_list_view_set_factory (self->list_view, factory);
  g_object_unref (factory);

  gtk_stack_set_visible_child_name (self->stack, "empty");
}

GtkWidget *
editor_folder_panel_new (void)
{
  return g_object_new (EDITOR_TYPE_FOLDER_PANEL, NULL);
}

GFile *
editor_folder_panel_get_folder (EditorFolderPanel *self)
{
  g_return_val_if_fail (EDITOR_IS_FOLDER_PANEL (self), NULL);

  return self->folder;
}

void
editor_folder_panel_set_folder (EditorFolderPanel *self,
                                GFile             *folder)
{
  g_return_if_fail (EDITOR_IS_FOLDER_PANEL (self));
  g_return_if_fail (folder == NULL || G_IS_FILE (folder));

  if (self->folder == folder)
    return;

  if (self->folder != NULL && folder != NULL && g_file_equal (self->folder, folder))
    return;

  g_set_object (&self->folder, folder);

  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");

  if (folder == NULL)
    {
      gtk_label_set_text (self->folder_name_label, _("No Folder Open"));
      gtk_widget_set_tooltip_text (GTK_WIDGET (self->folder_name_label), NULL);
      if (self->list_view != NULL)
        gtk_list_view_set_model (self->list_view, NULL);
      g_clear_object (&self->tree_model);
      gtk_stack_set_visible_child_name (self->stack, "empty");
    }
  else
    {
      g_autofree char *basename = g_file_get_basename (folder);
      g_autofree char *path = g_file_get_parse_name (folder);
      GListModel *root_model;
      GtkNoSelection *selection;

      gtk_label_set_text (self->folder_name_label, basename ? basename : path);
      gtk_widget_set_tooltip_text (GTK_WIDGET (self->folder_name_label), path);

      if (self->list_view != NULL)
        gtk_list_view_set_model (self->list_view, NULL);
      g_clear_object (&self->tree_model);

      root_model = create_folder_model (self, folder);
      self->tree_model = gtk_tree_list_model_new (root_model, FALSE, FALSE, create_tree_model_func, self, NULL);

      selection = gtk_no_selection_new (g_object_ref (G_LIST_MODEL (self->tree_model)));
      gtk_list_view_set_model (self->list_view, GTK_SELECTION_MODEL (selection));
      g_object_unref (selection);

      gtk_stack_set_visible_child_name (self->stack, "browser");
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FOLDER]);
}
