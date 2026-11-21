/*
obs-playlister
Copyright (C) 2026 Jeffrey A. Brick

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* URLs and Windows paths as playlists give them (R-REM-1). Text only: nothing here touches the disk or the network.
 * Every returned string is owned by the caller (bmem). */

/* The length of the scheme that text starts with (RFC 3986: a letter, then letters, digits, "+", "-" or ".", then
 * ":"), or 0. A one-letter scheme is a drive letter ("C:"), so it is never a scheme here. */
size_t pl_url_scheme_length(const char *text);
/* Whether text is an http:// or https:// URL, in any case */
bool pl_url_is_http(const char *text);

/* The extension of the last path segment, lower-cased (ASCII), without its dot; NULL when there is none. For a URL
 * the query and fragment are removed first; a local path keeps "?" and "#", which file names may contain. */
char *pl_url_path_extension(const char *location);

/* The location as a log line shows it, as URLs can carry credentials or signed query strings: for text with "://", the
 * scheme and "://", the authority from after its last '@' (user information may itself contain '@') and the path up
 * to the first '?' or '#'; anything else, such as a local path, unchanged. A secret inside the path still shows.
 *
 * The address is read by its structure (RFC 3986): the authority is the text between "://" and the first '/', '?' or
 * '#', and the user information what stands before the last '@' inside it. A valid address is never changed beyond
 * that: an '@' or ':' in its path or query, a port and an IPv6 host stay as they are.
 *
 * One kind of text that is no address is met half way (R-UX-23): a user name or password typed with a '/', '?' or '#'
 * that is not written as "%XX" ends the authority early. When the authority then has no '@', is not empty and is no
 * host with or without a port ("user:pa" of "https://user:pa/ss@host/x": "pa" is no port; a port without a host, as in
 * "srt://:9000", is one), everything up to the last
 * '@' before the first '?' or '#' is left out as user information, or up to the last '@' of the text when none stands
 * there: "https://host/x". Where the cut-off authority does read as a host ("user:12" of "https://user:12/34@host/x",
 * "token" of "https://token/more@host/x") the text is left as it is, and shows. */
char *pl_url_redacted(const char *location);

/* Free text that may hold addresses, such as a playlist entry's title, with each of them redacted
 * (pl_url_redacted): the text is read word by word, a word being what stands between spaces or tabs, so that every
 * address in it is seen and the words around one are left alone ("Live https://x.tv @ 8pm" stays as it is). Text
 * without "://" comes back unchanged. Never NULL. */
char *pl_text_redacted(const char *text);

/* A location as a person reads it, in two parts (R-UX-19): the name, which is its last part (the file name; of an
 * address with no path, the host), and the container, which is what stands before the name without the separator
 * between them (the folder, or the address up to there; "/" for a file in the root; NULL when nothing stands before
 * the name). The location is redacted first (pl_url_redacted), and separators at its end are left out. An address
 * without a host keeps the root of its path as the container ("file:///a.mp4" gives "a.mp4" and "file:///"), and
 * one that is nothing but its root is its own name ("file:///"). Neither is ever NULL but the container. */
void pl_location_split(const char *location, char **name, char **container);

/* The label of an item in a list a person picks from: the name first, then the container in brackets, as a narrow
 * list cuts a text at its end; a title, when the item has one of its own, before both.
 * "D:/shows/loops/clip-14.mp4" gives "clip-14.mp4 (D:/shows/loops)", with the title "Intro",
 * "Intro - clip-14.mp4 (D:/shows/loops)". title may be NULL; one that is or holds addresses is redacted
 * (pl_text_redacted). Never NULL: "" when there is neither a title nor a name. */
char *pl_location_label(const char *title, const char *location);

/* RFC 3986 section 5.2: reference resolved against base, which must be an absolute URL; dot segments removed, scheme
 * and host lower-cased. A reference that is an absolute URL itself is normalized the same way. */
char *pl_url_resolve(const char *base, const char *reference);

/* "%XX" sequences decoded; a malformed one, and "%00", are kept as they are */
char *pl_url_percent_decode(const char *text);

/* A file: URL as a Windows path (file:///C:/a%20b.mp3 -> C:\a b.mp3, file://server/share/x -> \\server\share\x,
 * file://localhost/C:/x -> C:\x, file:/C:/x -> C:\x); NULL when text is not a file: URL or names no usable path */
char *pl_url_file_to_path(const char *text);

/* Windows path rules for an entry of a local playlist at base_path (an absolute path): an absolute reference (C:\x,
 * \\server\share\x) stands alone, \x is on base_path's drive or share, C:x is read as C:\x, anything else is relative
 * to base_path's folder. Separators become "\", repeated ones collapse, "." and ".." are resolved (never above the
 * root). A \\?\ or \\.\ reference is returned as it is. */
char *pl_path_resolve(const char *base_path, const char *reference);

/* R-LNX-10: a local playlist on Linux follows POSIX path rules. Whether path is a POSIX absolute path ("/x", not the
 * "//server" of a Windows share); the list parsers pick the rules by their base path with it. */
bool pl_path_is_posix(const char *path);

/* A file: URL as a POSIX path (file:///home/a%20b.mp3 -> /home/a b.mp3, file://localhost/x -> /x, file:/x -> /x);
 * NULL when text is not a file: URL, names another host, or has no absolute path */
char *pl_url_file_to_posix_path(const char *text);

/* POSIX path rules for an entry of a local playlist at base_path (an absolute path): an absolute reference (/x)
 * stands alone, anything else is relative to base_path's folder. Repeated "/" collapse, "." and ".." are resolved
 * (never above "/"), and a backslash is an ordinary character. */
char *pl_posix_path_resolve(const char *base_path, const char *reference);

/* A Content-Type header value in its two parts (R-REM-1, R-REM-5): the media type lower-cased, without parameters,
 * and its charset parameter lower-cased, without quotes. Each is NULL when the header has none; a charset holding
 * anything but letters, digits, "-", "_", ".", ":" and "+", or longer than 40 characters, counts as none.
 * "Audio/X-MpegURL; charset=UTF-8" gives "audio/x-mpegurl" and "utf-8". header may be NULL. */
void pl_content_type_split(const char *header, char **type, char **charset);

/* An address without the user name and password it carries, and those two apart, "%XX" decoded: for a fetcher that
 * must be told them separately, so that it can keep them from another host a redirect names.
 * "https://us%40er:pw@host/x" gives "https://host/x", "us@er" and "pw". The user information is what stands between
 * "://" and the last "@" before the path; the password what follows its first ":". *user and *password are NULL when
 * the address has no user name (and *password is "" when it has a user name alone); *plain is always set. */
void pl_url_split_credentials(const char *url, char **plain, char **user, char **password);

/* Whether two addresses have the same scheme, host and port, as they are written (a port left out and the same port
 * written out count as different, which errs on the side of "no"). User names, passwords, paths and queries do not
 * count. */
bool pl_url_same_origin(const char *a, const char *b);

/* The address a fetch ended at, with the user name and password of the address it was given put back in when the two
 * have the same origin (pl_url_same_origin) and the final one carries none of its own; otherwise a copy of the final
 * one. A fetcher follows redirects with the credentials held apart from the address, so the address it reports has
 * none; the entries of a list resolve against that address and are played from the same server, which asks for them
 * again. To another origin they are never carried. */
char *pl_url_carry_credentials(const char *given, const char *final);
