/* SPDX-License-Identifier: MIT */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace openfg::config {

/// Effective layer configuration.
///
/// The file format is a deliberately small TOML subset: sections, string,
/// integer, float and boolean values, plus single-line arrays of strings. It
/// is exactly what `conf.toml` needs and nothing more, so the layer stays
/// dependency-free.
struct Config {
    bool enabled{false};
    std::string gpu{"auto"};
    bool crossGpu{false};
    unsigned multiplier{2};
    bool debug{false};

    /// Process names (basenames, `*` and `?` wildcards) the layer may run in.
    /// An empty include list means every process.
    std::vector<std::string> include;
    /// Process names that always fall back to passthrough.
    std::vector<std::string> exclude;
};

struct LoadResult {
    Config config;
    std::filesystem::path path;
    bool found{false};
    std::string error;
    std::vector<std::string> warnings;
};

/// `$OPENFG_CONFIG`, else `$XDG_CONFIG_HOME/openfg/conf.toml`, else
/// `$HOME/.config/openfg/conf.toml`.
std::filesystem::path defaultPath();

/// Parse a config file and apply `OPENFG_*` environment overrides. A missing
/// file is not an error; `result.found` is false and the defaults apply.
LoadResult load(const std::filesystem::path& path = {});

/// Parse TOML text (used by the loader and tests).
LoadResult parse(const std::string& text, const std::filesystem::path& path = {});

/// Validate an already-loaded config. Returns false and fills `error` on
/// invalid values (multiplier outside 2..4, empty GPU, bad pattern, ...).
bool validate(const Config& config, std::string& error);

/// Process-wide cached load, so the layer reads the file once. Errors are
/// reported through the result, not printed.
const LoadResult& processConfig();

/// Best-effort names for the current process: native executable, argv[0] and
/// any `.exe` paths mapped by Wine/Proton.
std::vector<std::string> processNames();

/// Include/exclude matching against `processNames()`.
bool processMatches(const Config& config);

/// Default file contents written by `openfg-cli init`.
std::string defaultFile();

/// Render the effective config as TOML (for `openfg-cli show`).
std::string toToml(const Config& config);

}
