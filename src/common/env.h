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
 *
 * Author: Robert Nagy, ronag89@gmail.com
 */

#pragma once

#include <boost/property_tree/ptree_fwd.hpp>

#include <string>
#include <vector>

namespace caspar { namespace env {

void configure(const std::wstring& filename);

const std::wstring& initial_folder();
const std::wstring& media_folder();
const std::wstring& log_folder();
const std::wstring& template_folder();
const std::wstring& data_folder();
const std::wstring& version();

bool log_to_file();

const boost::property_tree::wptree& properties();

// Logical CPU ids (from <configuration.cpu-affinity.realtime-cpus>) that latency-critical threads (video channels,
// GPU, decklink/NDI output) should be pinned to. Empty (the default) means no pinning is applied.
const std::vector<int>& realtime_cpu_affinity();

// Logical CPU ids (from <configuration.cpu-affinity.background-cpus>) that less time-critical threads (e.g. the
// ffmpeg file producer's decode threads) should be pinned to. Empty (the default) means no pinning is applied.
const std::vector<int>& background_cpu_affinity();

void log_configuration_warnings();

}} // namespace caspar::env
