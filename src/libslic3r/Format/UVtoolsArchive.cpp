///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "UVtoolsArchive.hpp"

#include <cstdlib>
#include <deque>
#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "libslic3r/Exception.hpp"

namespace Slic3r {

namespace fs = boost::filesystem;
namespace bp = boost::process;

namespace uvtools {

static const char *DefaultCmd = "/Applications/UVtools.app/Contents/MacOS/UVtoolsCmd";

std::string find_cmd()
{
    if (const char *env = std::getenv("UVTOOLS_CMD"); env && *env) {
        if (fs::exists(env))
            return env;
        throw ExportError(std::string("UVTOOLS_CMD points to a missing file: ") + env);
    }

    if (fs::exists(DefaultCmd))
        return DefaultCmd;

    fs::path found = bp::search_path("UVtoolsCmd");
    if (!found.empty())
        return found.string();

    throw ExportError("UVtools not found. Install it from github.com/sn4k3/UVtools or set UVTOOLS_CMD.");
}

static std::string last_lines(const fs::path &file, size_t count)
{
    std::ifstream in(file.string());
    std::deque<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
        if (lines.size() > count)
            lines.pop_front();
    }

    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

void convert(const std::string &cmd,
             const std::string &input,
             const std::string &encoder,
             const std::string &output)
{
    // Output goes to a file, not a pipe, so a chatty conversion can't block.
    fs::path log = fs::temp_directory_path() / fs::unique_path("uvtools-%%%%-%%%%.log");

    int code = -1;
    try {
        code = bp::system(bp::exe = cmd,
                          bp::args = std::vector<std::string>{"convert", input, encoder, output},
                          (bp::std_out & bp::std_err) > log);
    } catch (const std::exception &e) {
        throw ExportError("Failed to start UVtools (" + cmd + "): " + e.what());
    }

    const std::string tail = last_lines(log, 20);
    boost::system::error_code ec;
    fs::remove(log, ec);

    if (code != 0)
        throw ExportError("UVtools conversion to " + encoder + " failed (exit code " +
                          std::to_string(code) + "):\n" + tail);

    if (!fs::exists(output) || fs::file_size(output) == 0)
        throw ExportError("UVtools did not write " + output + ":\n" + tail);
}

} // namespace uvtools

void UVtoolsArchive::export_print(const std::string     fname,
                                  const SLAPrint       &print,
                                  const ThumbnailsList &thumbnails,
                                  const std::string    &projectname)
{
    // Fail early, before any file is written, if UVtools is missing.
    const std::string cmd = uvtools::find_cmd();

    fs::path work = fs::temp_directory_path() / fs::unique_path("uvtools-%%%%-%%%%");
    fs::create_directories(work);
    const fs::path sl1       = work / "print.sl1";
    const fs::path converted = work / ("print." + m_ext);

    SL1Archive::export_print(sl1.string(), print, thumbnails, projectname);

    try {
        uvtools::convert(cmd, sl1.string(), m_encoder, converted.string());
    } catch (const ExportError &e) {
        throw ExportError(std::string(e.what()) +
                          "\nThe intermediate SL1 file was kept at " + sl1.string());
    }

    // Copy only after a successful conversion, so a failure never damages
    // an existing file at the destination.
    try {
        fs::copy_file(converted, fname, fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error &e) {
        throw ExportError("Could not write " + fname + ": " + e.what());
    }

    boost::system::error_code ec;
    fs::remove_all(work, ec);
}

ArchiveEntry uvtools_sla_format(const char *id, const char *desc, const char *ext, const char *encoder)
{
    ArchiveEntry entry(id);
    entry.desc        = desc;
    entry.ext         = ext;
    entry.wrfactoryfn = [ext, encoder](const auto &cfg) {
        return std::make_unique<UVtoolsArchive>(cfg, encoder, ext);
    };
    // No reader: these formats are export-only.
    return entry;
}

} // namespace Slic3r
