///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_FORMAT_UVTOOLS_ARCHIVE_HPP
#define SLIC3R_FORMAT_UVTOOLS_ARCHIVE_HPP

#include <string>

#include "SL1.hpp"
#include "SLAArchiveFormatRegistry.hpp"

namespace Slic3r {

namespace uvtools {

// Resolves UVtoolsCmd: $UVTOOLS_CMD, then the default /Applications install,
// then PATH. Throws ExportError when none is found.
std::string find_cmd();

// Runs `UVtoolsCmd convert <input> <encoder> <output>`. Throws ExportError with
// the tool's last output lines when it fails or writes no output file.
// UVtools requires the output file extension to match the encoder.
void convert(const std::string &cmd,
             const std::string &input,
             const std::string &encoder,
             const std::string &output);

} // namespace uvtools

// Writes a regular SL1 archive to a temp folder and has UVtools convert it
// into a printer-native format such as .goo or .cxdlpv4.
class UVtoolsArchive : public SL1Archive {
    std::string m_encoder; // UVtools encoder name, e.g. "GooFile"
    std::string m_ext;     // file extension without the dot, e.g. "goo"

public:
    UVtoolsArchive(const SLAPrinterConfig &cfg, std::string encoder, std::string ext)
        : SL1Archive(cfg), m_encoder(std::move(encoder)), m_ext(std::move(ext)) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// Registry entry for an export-only format converted by UVtools.
ArchiveEntry uvtools_sla_format(const char *id, const char *desc, const char *ext, const char *encoder);

} // namespace Slic3r

#endif // SLIC3R_FORMAT_UVTOOLS_ARCHIVE_HPP
