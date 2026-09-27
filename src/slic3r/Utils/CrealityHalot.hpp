///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CrealityHalot_hpp_
#define slic3r_CrealityHalot_hpp_

#include <chrono>
#include <string>

#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Creality HALOT resin printers over WiFi, the way HalotBox sends files: a
// WebSocket on port 18188, with the password set on the printer
// (Settings > Print Settings > HALOT BOX > Password).
class CrealityHalot : public PrintHost
{
public:
    explicit CrealityHalot(DynamicPrintConfig *config);
    // host is "ip" or "ip:port".
    CrealityHalot(std::string host, std::string password,
                  std::chrono::milliseconds timeout = std::chrono::seconds(20));
    ~CrealityHalot() override = default;

    const char *get_name() const override;

    bool     test(wxString &msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool     upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    // Browse finds HALOT printers with HalotBox's UDP broadcast (not Bonjour).
    bool     has_auto_discovery() const override { return true; }
    bool     can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

private:
    std::string               m_host;
    std::string               m_password;
    std::chrono::milliseconds m_timeout;
    mutable std::string       m_status; // printer status seen by the last test()
};

} // namespace Slic3r

#endif
