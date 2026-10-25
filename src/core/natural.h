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

/* Natural order for names (R-DIR-6), so ep2 comes before ep10. A run of ASCII digits compares as a number: by its
 * length once leading zeros are stripped, then digit by digit, so no number is ever too long. Everything else
 * compares one byte at a time, ASCII letters without case; other bytes (the rest of UTF-8) by value. Names equal
 * under these rules (ep01 and ep1, Ep1 and ep1) compare equal; callers break that tie by source order.
 * Returns a negative number, 0 or a positive number. NULL sorts before every string. */
int pl_natural_compare(const char *a, const char *b);
