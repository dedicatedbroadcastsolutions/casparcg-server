/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

namespace caspar { namespace diagnostics {

// Registers a graph_sink that mirrors WARNING/INFO-severity set_tag() calls (e.g. "late-frame",
// "dropped-frame", "underflow", "audio-buffer-overflow") into the text log, so performance issues
// are visible there and not only in the visual diagnostics overlay. Rate-limited per (graph
// instance, tag name): the first occurrence logs immediately, further occurrences within the same
// window are counted and summarized in the next periodic log line, rather than spamming one line
// per occurrence.
void register_log_sink();

}} // namespace caspar::diagnostics
