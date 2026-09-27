///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CrealityHalot.hpp"

#include <boost/log/trivial.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "HalotClient.hpp"

namespace Slic3r {

namespace {

wxString message(const halot::Error &e)
{
    using Kind = halot::Error::Kind;
    switch (e.kind) {
    case Kind::timeout:
        return _L("The printer did not answer in time. Check that it is on and connected to the WiFi.");
    case Kind::password:
        return _L("The printer refused the password. Use the password set on the printer in "
                  "Settings > Print Settings > HALOT BOX > Password (123456 if you never changed it).");
    case Kind::busy:
        return _L("The printer is printing. Wait until it finishes, or upload without printing.");
    case Kind::refused:
        return GUI::format_wxstr(_L("The printer refused the file (%1%)."), e.what());
    case Kind::file:
        return GUI::format_wxstr(_L("Could not read the file to send: %1%"), e.what());
    case Kind::connection:
    default:
        return GUI::format_wxstr(_L("Connection to the printer failed: %1%"), e.what());
    }
}

// The printer's status in words, or as reported when unknown.
wxString status_text(const std::string &status)
{
    if (status == "PRINT_GENERAL" || status.empty())
        return _L("ready");
    if (status == "PRINT_PROCESSING")
        return _L("printing");
    if (status == "PRINT_STOP")
        return _L("paused");
    if (status == "PRINT_END" || status == "PRINT_COMPLETE")
        return _L("print finished");
    return wxString::FromUTF8(status);
}

} // namespace

CrealityHalot::CrealityHalot(DynamicPrintConfig *config)
    : CrealityHalot(config->opt_string("print_host"), config->opt_string("printhost_password"))
{}

CrealityHalot::CrealityHalot(std::string host, std::string password, std::chrono::milliseconds timeout)
    : m_host(std::move(host)), m_password(std::move(password)), m_timeout(timeout)
{}

const char *CrealityHalot::get_name() const { return "Creality Halot"; }

bool CrealityHalot::test(wxString &msg) const
{
    try {
        m_status = halot::Client(m_host, m_password, m_timeout).test();
        return true;
    } catch (const halot::Error &e) {
        msg = message(e);
        return false;
    } catch (const std::exception &e) {
        msg = message(halot::Error(halot::Error::Kind::connection, e.what()));
        return false;
    }
}

wxString CrealityHalot::get_test_ok_msg() const
{
    return GUI::format_wxstr(_L("Connection to the HALOT printer works correctly (status: %1%)."), status_text(m_status));
}

wxString CrealityHalot::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to the HALOT printer"), msg);
}

bool CrealityHalot::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn /*info_fn*/) const
{
    static const std::string no_body;
    try {
        const bool done = halot::Client(m_host, m_password, m_timeout)
            .upload(upload_data.source_path.string(), upload_data.upload_path.filename().string(),
                    upload_data.post_action == PrintHostPostUploadAction::StartPrint,
                    [&progress_fn](size_t sent, size_t total) {
                        bool cancel = false;
                        progress_fn(Http::Progress(0, 0, total, sent, no_body), cancel);
                        return !cancel;
                    });
        if (!done) {
            // Cancelled: tell the upload queue the way Http does, by calling
            // the progress function with cancel already set.
            bool cancel = true;
            progress_fn(Http::Progress(0, 0, 0, 0, no_body), cancel);
        }
        return done;
    } catch (const halot::Error &e) {
        BOOST_LOG_TRIVIAL(error) << "HALOT: " << e.what();
        error_fn(message(e));
        return false;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "HALOT: " << e.what();
        error_fn(message(halot::Error(halot::Error::Kind::connection, e.what())));
        return false;
    }
}

} // namespace Slic3r
