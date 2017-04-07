/* nmcli - command-line tool to control NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2010 - 2015 Red Hat, Inc.
 */

#include "nm-default.h"

#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "nm-client-utils.h"
#include "nm-meta-setting-access.h"

#include "common.h"
#include "settings.h"

#define ML_HEADER_WIDTH 79
#define ML_VALUE_INDENT 40

/*****************************************************************************/

static const char *
_meta_type_nmc_generic_info_get_name (const NMMetaAbstractInfo *abstract_info)
{
	return ((const NmcMetaGenericInfo *) abstract_info)->name;
}

static const NMMetaAbstractInfo *const*
_meta_type_nmc_generic_info_get_nested (const NMMetaAbstractInfo *abstract_info,
                                        guint *out_len,
                                        gpointer *out_to_free)
{
	const NmcMetaGenericInfo *info;
	guint n;

	info = (const NmcMetaGenericInfo *) abstract_info;

	if (out_len) {
		n = 0;
		if (info->nested) {
			for (; info->nested[n]; n++) {
			}
		}
		*out_len = n;
	}
	*out_to_free = NULL;
	return (const NMMetaAbstractInfo *const*) info->nested;
}

static gconstpointer
_meta_type_nmc_generic_info_get_fcn (const NMMetaEnvironment *environment,
                                     gpointer environment_user_data,
                                     const NMMetaAbstractInfo *abstract_info,
                                     gpointer target,
                                     NMMetaAccessorGetType get_type,
                                     NMMetaAccessorGetFlags get_flags,
                                     gpointer *out_to_free)
{
	const NmcMetaGenericInfo *info = (const NmcMetaGenericInfo *) abstract_info;

	nm_assert (out_to_free && !*out_to_free);

	if (!info->get_fcn)
		g_return_val_if_reached (NULL);
	return info->get_fcn (environment, environment_user_data,
	                      info, target,
	                      get_type, get_flags,
	                      out_to_free);
}

const NMMetaType nmc_meta_type_generic_info = {
	.type_name =         "nmc-generic-info",
	.get_name =          _meta_type_nmc_generic_info_get_name,
	.get_nested =        _meta_type_nmc_generic_info_get_nested,
	.get_fcn =           _meta_type_nmc_generic_info_get_fcn,
};

/*****************************************************************************/

static gboolean
parse_global_arg (NmCli *nmc, const char *arg)
{
	if (nmc_arg_is_option (arg, "ask"))
		nmc->ask = TRUE;
	else if (nmc_arg_is_option (arg, "show-secrets"))
		nmc->show_secrets = TRUE;
	else
		return FALSE;

	return TRUE;
}
/**
 * next_arg:
 * @nmc: NmCli data
 * @*argc: pointer to left number of arguments to parse
 * @***argv: pointer to const char *array of arguments still to parse
 * @...: a %NULL terminated list of cmd options to match (e.g., "--active")
 *
 * Takes care of autocompleting options when needed and performs
 * match against passed options while moving forward the pointer
 * to the remaining arguments.
 *
 * Returns: the number of the matched option  if a match is found against
 * one of the custom options passed; 0 if no custom option matched and still
 * some args need to be processed or autocompletion has been performed;
 * -1 otherwise (no more args).
 */
int
next_arg (NmCli *nmc, int *argc, char ***argv, ...)
{
	va_list args;
	const char *cmd_option;

	g_assert (*argc >= 0);

	do {
		int cmd_option_pos = 1;

		if (*argc > 0) {
			(*argc)--;
			(*argv)++;
		}
		if (*argc == 0)
			return -1;


		va_start (args, argv);

		if (nmc && nmc->complete && *argc == 1) {
			while ((cmd_option = va_arg (args, const char *)))
				nmc_complete_strings (**argv, cmd_option, NULL);

			if (***argv == '-')
				nmc_complete_strings (**argv, "--ask", "--show-secrets", NULL);

			va_end (args);
			return 0;
		}

		/* Check command dependent options first */
		while ((cmd_option = va_arg (args, const char *))) {
			/* strip heading "--" form cmd_option */
			if (nmc_arg_is_option (**argv, cmd_option + 2)) {
				va_end (args);
				return cmd_option_pos;
			}
			cmd_option_pos++;
		}

		va_end (args);

	} while (nmc && parse_global_arg (nmc, **argv));

	return 0;
}

gboolean
nmc_arg_is_help (const char *arg)
{
	if (!arg)
		return FALSE;
	if (   matches (arg, "help")
	    || (g_str_has_prefix (arg, "-")  && matches (arg + 1, "help"))
	    || (g_str_has_prefix (arg, "--") && matches (arg + 2, "help"))) {
		return TRUE;
	}
	return FALSE;
}

gboolean
nmc_arg_is_option (const char *str, const char *opt_name)
{
	const char *p;

	if (!str || !*str)
		return FALSE;

	if (str[0] != '-')
		return FALSE;

	p = (str[1] == '-') ? str + 2 : str + 1;

	return (*p ? matches (p, opt_name) : FALSE);
}

/*
 * Helper function to parse command-line arguments.
 * arg_arr: description of arguments to look for
 * last:    whether these are last expected arguments
 * argc:    command-line argument array size
 * argv:    command-line argument array
 * error:   error set on a failure (when FALSE is returned)
 * Returns: TRUE on success, FALSE on an error and sets 'error'
 */
gboolean
nmc_parse_args (nmc_arg_t *arg_arr, gboolean last, int *argc, char ***argv, GError **error)
{
	nmc_arg_t *p;
	gboolean found;
	gboolean have_mandatory;

	g_return_val_if_fail (arg_arr != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	while (*argc > 0) {
		found = FALSE;

		for (p = arg_arr; p->name; p++) {
			if (strcmp (**argv, p->name) == 0) {

				if (p->found) {
					/* Don't allow repeated arguments, because the argument of the same
					 * name could be used later on the line for another purpose. Assume
					 * that's the case and return.
					 */
					return TRUE;
				}

				if (p->has_value) {
					(*argc)--;
					(*argv)++;
					if (!*argc) {
						g_set_error (error, NMCLI_ERROR, NMC_RESULT_ERROR_USER_INPUT,
						             _("Error: value for '%s' argument is required."), *(*argv-1));
						return FALSE;
					}
					*(p->value) = **argv;
				}
				p->found = TRUE;
				found = TRUE;
				break;
			}
		}

		if (!found) {
			have_mandatory = TRUE;
			for (p = arg_arr; p->name; p++) {
				if (p->mandatory && !p->found) {
					have_mandatory = FALSE;
					break;
				}
			}

			if (have_mandatory && !last)
				return TRUE;

			if (p->name)
				g_set_error (error, NMCLI_ERROR, NMC_RESULT_ERROR_USER_INPUT,
				             _("Error: Argument '%s' was expected, but '%s' provided."), p->name, **argv);
			else
				g_set_error (error, NMCLI_ERROR, NMC_RESULT_ERROR_USER_INPUT,
				             _("Error: Unexpected argument '%s'"), **argv);
			return FALSE;
		}

		next_arg (NULL, argc, argv, NULL);
	}

	return TRUE;
}

/*
 *  Convert SSID to a hex string representation.
 *  Caller has to free the returned string using g_free()
 */
char *
ssid_to_hex (const char *str, gsize len)
{
	GString *printable;
	char *printable_str;
	int i;

	if (str == NULL || len == 0)
		return NULL;

	printable = g_string_new (NULL);
	for (i = 0; i < len; i++) {
		g_string_append_printf (printable, "%02X", (unsigned char) str[i]);
	}
	printable_str = g_string_free (printable, FALSE);
	return printable_str;
}

/*
 * Erase terminal line using ANSI escape sequences.
 * It prints <ESC>[2K sequence to erase the line and then \r to return back
 * to the beginning of the line.
 *
 * http://www.termsys.demon.co.uk/vtansi.htm
 */
void
nmc_terminal_erase_line (void)
{
	/* We intentionally use printf(), not g_print() here, to ensure that
	 * GLib doesn't mistakenly try to convert the string.
	 */
	printf ("\33[2K\r");
	fflush (stdout);
}

/*
 * Print animated progress for an operation.
 * Repeated calls of the function will show rotating slash in terminal followed
 * by the string passed in 'str' argument.
 */
void
nmc_terminal_show_progress (const char *str)
{
	static int idx = 0;
	const char slashes[4] = {'|', '/', '-', '\\'};

	nmc_terminal_erase_line ();
	g_print ("%c %s", slashes[idx++], str ? str : "");
	fflush (stdout);
	if (idx == 4)
		idx = 0;
}

const char *
nmc_term_color_sequence (NMMetaTermColor color)
{
	switch (color) {
        case NM_META_TERM_COLOR_BLACK:
		return "\33[30m";
		break;
        case NM_META_TERM_COLOR_RED:
		return "\33[31m";
		break;
        case NM_META_TERM_COLOR_GREEN:
		return "\33[32m";
		break;
        case NM_META_TERM_COLOR_YELLOW:
		return "\33[33m";
		break;
        case NM_META_TERM_COLOR_BLUE:
		return "\33[34m";
		break;
        case NM_META_TERM_COLOR_MAGENTA:
		return "\33[35m";
		break;
        case NM_META_TERM_COLOR_CYAN:
		return "\33[36m";
		break;
        case NM_META_TERM_COLOR_WHITE:
		return "\33[37m";
		break;
	default:
		return "";
		break;
	}
}

/* Parses @str for color as string or number */
NMMetaTermColor
nmc_term_color_parse_string (const char *str, GError **error)
{
	unsigned long color_int;
	static const char *colors[] = { "normal", "black", "red", "green", "yellow",
	                                "blue", "magenta", "cyan", "white", NULL };

	if (nmc_string_to_uint (str, TRUE, 0, 8, &color_int)) {
		return (NMMetaTermColor) color_int;
	} else {
		const char *color, **p;
		int i;

		color = nmc_string_is_valid (str, colors, error);
		for (p = colors, i = 0; *p != NULL; p++, i++) {
			if (*p == color)
				return (NMMetaTermColor) i;
		}
		return -1;
	}
}

const char *
nmc_term_format_sequence (NMMetaTermFormat format)
{
	switch (format) {
        case NM_META_TERM_FORMAT_BOLD:
		return "\33[1m";
		break;
        case NM_META_TERM_FORMAT_DIM:
		return "\33[2m";
		break;
        case NM_META_TERM_FORMAT_UNDERLINE:
		return "\33[4m";
		break;
        case NM_META_TERM_FORMAT_BLINK:
		return "\33[5m";
		break;
        case NM_META_TERM_FORMAT_REVERSE:
		return "\33[7m";
		break;
        case NM_META_TERM_FORMAT_HIDDEN:
		return "\33[8m";
		break;
	default:
		return "";
		break;
	}
}

static gboolean
use_colors (NmcColorOption color_option)
{
	if (color_option == NMC_USE_COLOR_AUTO) {
		static NmcColorOption cached = NMC_USE_COLOR_AUTO;

		if (G_UNLIKELY (cached == NMC_USE_COLOR_AUTO)) {
			if (   g_strcmp0 (g_getenv ("TERM"), "dumb") == 0
				|| !isatty (fileno (stdout)))
				cached = NMC_USE_COLOR_NO;
			else
				cached = NMC_USE_COLOR_YES;
		}
		return cached == NMC_USE_COLOR_YES;
	}

	return color_option == NMC_USE_COLOR_YES;
}

char *
nmc_colorize (NmcColorOption color_option, NMMetaTermColor color, NMMetaTermFormat format, const char *fmt, ...)
{
	va_list args;
	char *str, *colored;
	const char *ansi_color, *color_end, *ansi_fmt, *format_end;
	static const char *end_seq = "\33[0m";

	va_start (args, fmt);
	str = g_strdup_vprintf (fmt, args);
	va_end (args);

	if (!use_colors (color_option))
		return str;

	ansi_color = nmc_term_color_sequence (color);
	ansi_fmt = nmc_term_format_sequence (format);
	color_end = *ansi_color ? end_seq : "";
	format_end = *ansi_fmt ? end_seq : "";

	colored = g_strdup_printf ("%s%s%s%s%s", ansi_fmt, ansi_color, str, color_end, format_end);
	g_free (str);
	return colored;
}

/*
 * Count characters belonging to terminal color escape sequences.
 * @start points to beginning of the string, @end points to the end,
 * or NULL if the string is nul-terminated.
 */
static int
nmc_count_color_escape_chars (const char *start, const char *end)
{
	int num = 0;
	gboolean inside = FALSE;

	if (end == NULL)
		end = start + strlen (start);

	while (start < end) {
		if (*start == '\33' && *(start+1) == '[')
			inside = TRUE;
		if (inside)
			num++;
		if (*start == 'm') 
			inside = FALSE;
		start++;
	}
	return num;
}

/* Filter out possible ANSI color escape sequences */
/* It directly modifies the passed string @str. */
void
nmc_filter_out_colors_inplace (char *str)
{
	const char *p1;
	char *p2;
	gboolean copy_char = TRUE;

	if (!str)
		return;

	p1 = p2 = str;
	while (*p1) {
		if (*p1 == '\33' && *(p1+1) == '[')
			copy_char = FALSE;
		if (copy_char)
			*p2++ = *p1;
		if (!copy_char && *p1 == 'm')
			copy_char = TRUE;
		p1++;
	}
	*p2 = '\0';
}

/* Filter out possible ANSI color escape sequences */
char *
nmc_filter_out_colors (const char *str)
{
	char *filtered;

	if (!str)
		return NULL;

	filtered = g_strdup (str);
	nmc_filter_out_colors_inplace (filtered);
	return filtered;
}

/*
 * Ask user for input and return the string.
 * The caller is responsible for freeing the returned string.
 */
char *
nmc_get_user_input (const char *ask_str)
{
	char *line = NULL;
	size_t line_ln = 0;
	ssize_t num;

	g_print ("%s", ask_str);
	num = getline (&line, &line_ln, stdin);

	/* Remove newline from the string */
	if (num < 1 || (num == 1 && line[0] == '\n')) {
		g_free (line);
		line = NULL;
	} else {
		if (line[num-1] == '\n')
			line[num-1] = '\0';
	}

	return line;
}

/*
 * Split string in 'line' according to 'delim' to (argument) array.
 */
int
nmc_string_to_arg_array (const char *line, const char *delim, gboolean unquote,
                         char ***argv, int *argc)
{
	char **arr;

	arr = nmc_strsplit_set (line ? line : "", delim ? delim : " \t", 0);

	if (unquote) {
		int i = 0;
		char *s;
		size_t l;
		const char *quotes = "\"'";

		while (arr && arr[i]) {
			s = arr[i];
			l = strlen (s);
			if (l >= 2) {
				if (strchr (quotes, s[0]) && s[l-1] == s[0]) {
					memmove (s, s+1, l-2);
					s[l-2] = '\0';
				}
			}
			i++;
		}
	}

	*argv = arr;
	*argc = g_strv_length (arr);

	return 0;
}

/*
 * Convert string array (char **) to description string in the form of:
 * "[string1, string2, ]"
 *
 * Returns: a newly allocated string. Caller must free it with g_free().
 */
char *
nmc_util_strv_for_display (const char *const*strv, gboolean brackets)
{
	GString *result;
	guint i = 0;

	result = g_string_sized_new (150);
	if (brackets)
		g_string_append_c (result, '[');
	while (strv && strv[i]) {
		if (result->len > 1)
			g_string_append (result, ", ");
		g_string_append (result, strv[i]);
		i++;
	}
	if (brackets)
		g_string_append_c (result, ']');

	return g_string_free (result, FALSE);
}

/*
 * Find out how many columns an UTF-8 string occupies on the screen.
 */
int
nmc_string_screen_width (const char *start, const char *end)
{
	int width = 0;
	const char *p = start;

	if (end == NULL)
		end = start + strlen (start);

	while (p < end) {
		width += g_unichar_iswide (g_utf8_get_char (p)) ? 2 : g_unichar_iszerowidth (g_utf8_get_char (p)) ? 0 : 1;
		p = g_utf8_next_char (p);
	}

	/* Subtract color escape sequences as they don't occupy space. */
	return width - nmc_count_color_escape_chars (start, NULL);
}

void
set_val_str (NmcOutputField fields_array[], guint32 idx, char *value)
{
	fields_array[idx].value = value;
	fields_array[idx].value_is_array = FALSE;
	fields_array[idx].free_value = TRUE;
}

void
set_val_strc (NmcOutputField fields_array[], guint32 idx, const char *value)
{
	fields_array[idx].value = (char *) value;
	fields_array[idx].value_is_array = FALSE;
	fields_array[idx].free_value = FALSE;
}

void
set_val_arr (NmcOutputField fields_array[], guint32 idx, char **value)
{
	fields_array[idx].value = value;
	fields_array[idx].value_is_array = TRUE;
	fields_array[idx].free_value = TRUE;
}

void
set_val_arrc (NmcOutputField fields_array[], guint32 idx, const char **value)
{
	fields_array[idx].value = (char **) value;
	fields_array[idx].value_is_array = TRUE;
	fields_array[idx].free_value = FALSE;
}

void
set_val_color_all (NmcOutputField fields_array[], NMMetaTermColor color)
{
	int i;

	for (i = 0; fields_array[i].info; i++) {
		fields_array[i].color = color;
	}
}

void
set_val_color_fmt_all (NmcOutputField fields_array[], NMMetaTermFormat format)
{
	int i;

	for (i = 0; fields_array[i].info; i++) {
		fields_array[i].color_fmt = format;
	}
}

/*
 * Free 'value' members in array of NmcOutputField
 */
void
nmc_free_output_field_values (NmcOutputField fields_array[])
{
	NmcOutputField *iter = fields_array;

	while (iter && iter->info) {
		if (iter->free_value) {
			if (iter->value_is_array)
				g_strfreev ((char **) iter->value);
			else
				g_free ((char *) iter->value);
			iter->value = NULL;
		}
		iter++;
	}
}

typedef struct {
	guint idx;
	gsize offset_plus_1;
} OutputSelectionItem;

NmcOutputSelection *
nmc_output_selection_create (const NMMetaAbstractInfo *const* fields_array,
                             const char *fields_str,
                             GError **error)
{
	gs_unref_array GArray *array = NULL;
	nm_auto_free_gstring GString *str = NULL;
	guint i, j;
	NmcOutputSelection *result;

	g_return_val_if_fail (!error || !*error, NULL);

	array = g_array_new (FALSE, FALSE, sizeof (OutputSelectionItem));

	if (!fields_str) {
		for (i = 0; fields_array[i]; i++) {
			OutputSelectionItem s = {
				.idx = i,
			};

			g_array_append_val (array, s);
		}
	} else {
		gs_free char *fields_str_clone = NULL;
		char *fields_str_cur;
		char *fields_str_next;

		fields_str_clone = g_strdup (fields_str);
		for (fields_str_cur = fields_str_clone; fields_str_cur; fields_str_cur = fields_str_next) {
			const char *i_name;
			const char *right = NULL;
			gboolean found = FALSE;
			const NMMetaAbstractInfo *fields_array_failure = NULL;

			fields_str_cur = nm_str_skip_leading_spaces (fields_str_cur);
			fields_str_next = strchr (fields_str_cur, ',');
			if (fields_str_next)
				*fields_str_next++ = '\0';

			g_strchomp (fields_str_cur);
			if (!fields_str_cur[0])
				continue;

			i_name = fields_str_cur;
			fields_str_cur = strchr (fields_str_cur, '.');
			if (fields_str_cur) {
				right = fields_str_cur + 1;
				*fields_str_cur = '\0';
			}

			for (i = 0; fields_array[i]; i++) {
				const NMMetaAbstractInfo *fi = fields_array[i];

				if (g_ascii_strcasecmp (i_name, nm_meta_abstract_info_get_name (fi)) != 0)
					continue;

				if (!right)
					found = TRUE;
				else {
					found = FALSE;
					if (fi->meta_type == &nm_meta_type_setting_info_editor) {
						const NMMetaSettingInfoEditor *fi_s = &fi->as.setting_info;

						for (j = 1; j < fi_s->properties_num; j++) {
							if (g_ascii_strcasecmp (right, fi_s->properties[j].property_name) == 0) {
								found = TRUE;
								break;
							}
						}
					} else if (fi->meta_type == &nmc_meta_type_generic_info) {
						const NmcMetaGenericInfo *fi_g = (const NmcMetaGenericInfo *) fi;

						for (j = 0; fi_g->nested && fi_g->nested[j]; j++) {
							if (g_ascii_strcasecmp (right, nm_meta_abstract_info_get_name ((const NMMetaAbstractInfo *) fi_g->nested[j])) == 0) {
								found = TRUE;
								break;
							}
						}
					}
				}

				if (found) {
					OutputSelectionItem s = {
						.idx = i,
					};

					if (right) {
						if (!str)
							str = g_string_sized_new (32);

						s.offset_plus_1 = str->len + 1;
						g_string_append_len (str, right, strlen (right) + 1);
					}

					g_array_append_val (array, s);
				}

				fields_array_failure = fields_array[i];
				break;
			}

			if (!found) {
				if (   !right
				    && (   !g_ascii_strcasecmp (i_name, "all")
				        || !g_ascii_strcasecmp (i_name, "common")))
					g_set_error (error, NMCLI_ERROR, 0, _("field '%s' has to be alone"), i_name);
				else {
					gs_free char *allowed_fields = NULL;

					if (fields_array_failure)
						allowed_fields = nmc_get_allowed_fields_nested (fields_array_failure);
					else
						allowed_fields = nmc_get_allowed_fields (fields_array);

					g_set_error (error, NMCLI_ERROR, 1, _("invalid field '%s%s%s'; allowed fields: %s"),
					             i_name, right ? "." : "", right ?: "", allowed_fields);
				}

				return NULL;
			}
		}
	}

	/* re-organize the collected output data in one buffer that can be freed using
	 * g_free(). This makes allocation more complicated, but saves us from special
	 * handling for free. */
	result = g_malloc0 (sizeof (NmcOutputSelection) + (array->len * sizeof (NmcOutputSelectionItem)) + (str ? str->len : 0));
	*((guint *) &result->num) = array->len;
	if (array->len > 0) {
		char *pdata = &((char *) result)[sizeof (NmcOutputSelection) + (array->len * sizeof (NmcOutputSelectionItem))];

		if (str)
			memcpy (pdata, str->str, str->len);
		for (i = 0; i < array->len; i++) {
			const OutputSelectionItem *a = &g_array_index (array, OutputSelectionItem, i);
			NmcOutputSelectionItem *p = (NmcOutputSelectionItem *) &result->items[i];

			p->info = fields_array[a->idx];
			p->idx = a->idx;
			if (a->offset_plus_1 > 0)
				p->sub_selection = &pdata[a->offset_plus_1 - 1];
		}
	}

	return result;
}


/**
 * parse_output_fields:
 * @field_str: comma-separated field names to parse
 * @fields_array: array of allowed fields
 * @parse_groups: whether the fields can contain group prefix (e.g. general.driver)
 * @group_fields: (out) (allow-none): array of field names for particular groups
 * @error: (out) (allow-none): location to store error, or %NULL
 *
 * Parses comma separated fields in @fields_str according to @fields_array.
 * When @parse_groups is %TRUE, fields can be in the form 'group.field'. Then
 * @group_fields will be filled with the required field for particular group.
 * @group_fields array corresponds to the returned array.
 * Examples:
 *   @field_str:     "type,name,uuid" | "ip4,general.device" | "ip4.address,ip6"
 *   returned array:   2    0    1    |   7         0        |     7         9
 *   @group_fields:   NULL NULL NULL  |  NULL    "device"    | "address"    NULL
 *
 * Returns: #GArray with indices representing fields in @fields_array.
 *   Caller is responsible for freeing the array.
 */
GArray *
parse_output_fields (const char *fields_str,
                     const NMMetaAbstractInfo *const*fields_array,
                     gboolean parse_groups,
                     GPtrArray **out_group_fields,
                     GError **error)
{
	gs_free NmcOutputSelection *selection = NULL;
	GArray *array;
	GPtrArray *group_fields = NULL;
	guint i;

	g_return_val_if_fail (!error || !*error, NULL);
	g_return_val_if_fail (!out_group_fields || !*out_group_fields, NULL);

	selection = nmc_output_selection_create (fields_array, fields_str, error);
	if (!selection)
		return NULL;

	array = g_array_sized_new (FALSE, FALSE, sizeof (int), selection->num);
	if (parse_groups && out_group_fields)
		group_fields = g_ptr_array_new_full (selection->num, g_free);

	for (i = 0; i < selection->num; i++) {
		int idx = selection->items[i].idx;

		g_array_append_val (array, idx);
		if (group_fields)
			g_ptr_array_add (group_fields, g_strdup (selection->items[i].sub_selection));
	}

	if (group_fields)
		*out_group_fields = group_fields;
	return array;
}

char *
nmc_get_allowed_fields_nested (const NMMetaAbstractInfo *abstract_info)
{
	GString *allowed_fields = g_string_sized_new (256);
	int i;
	const char *name = nm_meta_abstract_info_get_name (abstract_info);
	gs_free gpointer nested_to_free = NULL;
	const NMMetaAbstractInfo *const*nested = NULL;

	nested = nm_meta_abstract_info_get_nested (abstract_info, NULL, &nested_to_free);
	if (nested) {
		for (i = 0; nested && nested[i]; i++) {
			g_string_append_printf (allowed_fields, "%s.%s,",
			                        name, nm_meta_abstract_info_get_name (nested[i]));
		}
	} else
		g_string_append_printf (allowed_fields, "%s,", name);

	g_string_truncate (allowed_fields, allowed_fields->len - 1);

	return g_string_free (allowed_fields, FALSE);
}

char *
nmc_get_allowed_fields (const NMMetaAbstractInfo *const*fields_array)
{
	GString *allowed_fields = g_string_sized_new (256);
	guint i;

	for (i = 0; fields_array[i]; i++)
		g_string_append_printf (allowed_fields, "%s,", nm_meta_abstract_info_get_name (fields_array[i]));

	if (allowed_fields->len)
		g_string_truncate (allowed_fields, allowed_fields->len - 1);

	return g_string_free (allowed_fields, FALSE);
}

NmcOutputField *
nmc_dup_fields_array (const NMMetaAbstractInfo *const*fields, NmcOfFlags flags)
{
	NmcOutputField *row;
	gsize l;

	for (l = 0; fields[l]; l++) {
	}

	row = g_new0 (NmcOutputField, l + 1);
	for (l = 0; fields[l]; l++)
		row[l].info = fields[l];
	row[0].flags = flags;
	return row;
}

void
nmc_empty_output_fields (NmcOutputData *output_data)
{
	guint i;

	/* Free values in field structure */
	for (i = 0; i < output_data->output_data->len; i++) {
		NmcOutputField *fld_arr = g_ptr_array_index (output_data->output_data, i);
		nmc_free_output_field_values (fld_arr);
	}

	/* Empty output_data array */
	if (output_data->output_data->len > 0)
		g_ptr_array_remove_range (output_data->output_data, 0, output_data->output_data->len);
}

static const char *
colorize_string (NmcColorOption color_option,
                 NMMetaTermColor color,
                 NMMetaTermFormat color_fmt,
                 const char *str,
                 char **out_to_free)
{
	const char *out = str;

	if (   use_colors (color_option)
	    && (color != NM_META_TERM_COLOR_NORMAL || color_fmt != NM_META_TERM_FORMAT_NORMAL)) {
		*out_to_free = nmc_colorize (color_option, color, color_fmt, "%s", str);
		out = *out_to_free;
	}

	return out;
}

static const char *
get_value_to_print (NmcColorOption color_option,
                    const NmcOutputField *field,
                    gboolean field_name,
                    const char *not_set_str,
                    char **out_to_free)
{
	gboolean is_array = field->value_is_array;
	const char *value;
	const char *out;
	gs_free char *free_value = NULL;

	nm_assert (out_to_free && !*out_to_free);

	if (field_name)
		value = _(nm_meta_abstract_info_get_name (field->info));
	else {
		value = field->value
		            ? (is_array
		                  ? (free_value = g_strjoinv (" | ", (char **) field->value))
		                  : (*((const char *) field->value))
		                        ? field->value
		                        : not_set_str)
		            : not_set_str;
	}

	/* colorize the value */
	out = colorize_string (color_option, field->color, field->color_fmt, value, out_to_free);

	if (out && out == free_value) {
		nm_assert (!*out_to_free);
		*out_to_free = g_steal_pointer (&free_value);
	}

	return out;
}

/*
 * Print both headers or values of 'field_values' array.
 * Entries to print and their order are specified via indices in
 * 'nmc->indices' array.
 * Various flags influencing the output of fields are set up in the first item
 * of 'field_values' array.
 */
void
print_required_fields (const NmcConfig *nmc_config,
                       NmcOfFlags of_flags,
                       const GArray *indices,
                       const char *header_name,
                       int indent,
                       const NmcOutputField *field_values)
{
	nm_auto_free_gstring GString *str = NULL;
	int width1, width2;
	int table_width = 0;
	const char *not_set_str;
	int i;
	gboolean terse = (nmc_config->print_output == NMC_PRINT_TERSE);
	gboolean pretty = (nmc_config->print_output == NMC_PRINT_PRETTY);
	gboolean main_header_add = of_flags & NMC_OF_FLAG_MAIN_HEADER_ADD;
	gboolean main_header_only = of_flags & NMC_OF_FLAG_MAIN_HEADER_ONLY;
	gboolean field_names = of_flags & NMC_OF_FLAG_FIELD_NAMES;
	gboolean section_prefix = of_flags & NMC_OF_FLAG_SECTION_PREFIX;

	/* --- Main header --- */
	if ((main_header_add || main_header_only) && pretty) {
		gs_free char *line = NULL;
		int header_width;

		header_width = nmc_string_screen_width (header_name, NULL) + 4;

		if (nmc_config->multiline_output) {
			table_width = NM_MAX (header_width, ML_HEADER_WIDTH);
			line = g_strnfill (ML_HEADER_WIDTH, '=');
		} else { /* tabular */
			table_width = NM_MAX (table_width, header_width);
			line = g_strnfill (table_width, '=');
		}

		width1 = strlen (header_name);
		width2 = nmc_string_screen_width (header_name, NULL);
		g_print ("%s\n", line);
		g_print ("%*s\n", (table_width + width2)/2 + width1 - width2, header_name);
		g_print ("%s\n", line);
	}

	if (main_header_only)
		return;

	/* No field headers are printed in terse mode nor for multiline output */
	if ((terse || nmc_config->multiline_output) && field_names)
		return;

	/* Don't replace empty strings in terse mode */
	not_set_str = terse ? "" : "--";

	if (nmc_config->multiline_output) {
		for (i = 0; i < indices->len; i++) {
			int idx = g_array_index (indices, int, i);
			gboolean is_array = field_values[idx].value_is_array;

			/* section prefix can't be an array */
			g_assert (!is_array || !section_prefix || idx != 0);

			if (section_prefix && idx == 0)  /* The first field is section prefix */
				continue;

			if (is_array) {
				gs_free char *val_to_free = NULL;
				const char **p, *val, *print_val;
				int j;

				/* value is a null-terminated string array */

				for (p = (const char **) field_values[idx].value, j = 1; p && *p; p++, j++) {
					gs_free char *tmp = NULL;

					val = *p ?: not_set_str;
					print_val = colorize_string (nmc_config->use_colors, field_values[idx].color, field_values[idx].color_fmt,
					                             val, &val_to_free);
					tmp = g_strdup_printf ("%s%s%s[%d]:",
					                       section_prefix ? (const char*) field_values[0].value : "",
					                       section_prefix ? "." : "",
					                       _(nm_meta_abstract_info_get_name (field_values[idx].info)),
					                       j);
					width1 = strlen (tmp);
					width2 = nmc_string_screen_width (tmp, NULL);
					g_print ("%-*s%s\n", terse ? 0 : ML_VALUE_INDENT+width1-width2, tmp, print_val);
				}
			} else {
				gs_free char *val_to_free = NULL;
				gs_free char *tmp = NULL;
				const char *hdr_name = (const char*) field_values[0].value;
				const char *val = (const char*) field_values[idx].value;
				const char *print_val;

				/* value is a string */

				val = val && *val ? val : not_set_str;
				print_val = colorize_string (nmc_config->use_colors, field_values[idx].color, field_values[idx].color_fmt,
				                             val, &val_to_free);
				tmp = g_strdup_printf ("%s%s%s:",
				                       section_prefix ? hdr_name : "",
				                       section_prefix ? "." : "",
				                       _(nm_meta_abstract_info_get_name (field_values[idx].info)));
				width1 = strlen (tmp);
				width2 = nmc_string_screen_width (tmp, NULL);
				g_print ("%-*s%s\n", terse ? 0 : ML_VALUE_INDENT+width1-width2, tmp, print_val);
			}
		}
		if (pretty) {
			gs_free char *line = NULL;

			g_print ("%s\n", (line = g_strnfill (ML_HEADER_WIDTH, '-')));
		}

		return;
	}

	/* --- Tabular mode: each line = one object --- */

	str = g_string_new (NULL);

	for (i = 0; i < indices->len; i++) {
		gs_free char *val_to_free = NULL;
		int idx;
		const char *value;

		idx = g_array_index (indices, int, i);

		value = get_value_to_print (nmc_config->use_colors, (NmcOutputField *) field_values+idx, field_names,
		                            not_set_str, &val_to_free);

		if (terse) {
			if (nmc_config->escape_values) {
				const char *p = value;
				while (*p) {
					if (*p == ':' || *p == '\\')
						g_string_append_c (str, '\\');  /* Escaping by '\' */
					g_string_append_c (str, *p);
					p++;
				}
			}
			else
				g_string_append_printf (str, "%s", value);
			g_string_append_c (str, ':');  /* Column separator */
		} else {
			width1 = strlen (value);
			width2 = nmc_string_screen_width (value, NULL);  /* Width of the string (in screen colums) */
			g_string_append_printf (str, "%-*s", field_values[idx].width + width1 - width2, strlen (value) > 0 ? value : not_set_str);
			g_string_append_c (str, ' ');  /* Column separator */
			table_width += field_values[idx].width + width1 - width2 + 1;
		}
	}

	/* Print actual values */
	if (str->len > 0) {
		g_string_truncate (str, str->len-1);  /* Chop off last column separator */
		if (indent > 0) {
			gs_free char *indent_str = NULL;

			g_string_prepend (str, (indent_str = g_strnfill (indent, ' ')));
		}

		g_print ("%s\n", str->str);

		/* Print horizontal separator */
		if (field_names && pretty) {
			gs_free char *line = NULL;

			g_print ("%s\n", (line = g_strnfill (table_width, '-')));
		}
	}
}

void
print_data_prepare_width (GPtrArray *output_data)
{
	int i, j;
	size_t len;
	NmcOutputField *row;
	int num_fields = 0;

	if (!output_data || output_data->len < 1)
		return;

	/* How many fields? */
	row = g_ptr_array_index (output_data, 0);
	while (row->info) {
		num_fields++;
		row++;
	}

	/* Find out maximal string lengths */
	for (i = 0; i < num_fields; i++) {
		size_t max_width = 0;
		for (j = 0; j < output_data->len; j++) {
			gboolean field_names;
			gs_free char * val_to_free = NULL;
			const char *value;

			row = g_ptr_array_index (output_data, j);
			field_names = row[0].flags & NMC_OF_FLAG_FIELD_NAMES;
			value = get_value_to_print (NMC_USE_COLOR_NO, row+i, field_names, "--", &val_to_free);
			len = nmc_string_screen_width (value, NULL);
			max_width = len > max_width ? len : max_width;
		}
		for (j = 0; j < output_data->len; j++) {
			row = g_ptr_array_index (output_data, j);
			row[i].width = max_width + 1;
		}
	}
}

void
print_data (const NmcConfig *nmc_config,
            const GArray *indices,
            const char *header_name,
            int indent,
            const NmcOutputData *out)
{
	guint i;

	for (i = 0; i < out->output_data->len; i++) {
		const NmcOutputField *field_values = g_ptr_array_index (out->output_data, i);

		print_required_fields (nmc_config, field_values[0].flags,
		                       indices, header_name,
		                       indent, field_values);
	}
}

