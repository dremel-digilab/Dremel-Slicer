// BonjourDialog.cpp
// Windows note: boost must be included before wxWidgets.

#include "slic3r/Utils/Bonjour.hpp"
#include "BonjourDialog.hpp"

#include <set>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <ctime>

#include <boost/asio.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/Bonjour.hpp"

namespace Slic3r {

constexpr uint16_t kMoonrakerPort = 7125;
constexpr int      kConnectTimeoutMs = 400; // TCP connect timeout
constexpr int      kHttpTimeoutMs    = 600; // HTTP read timeout
constexpr int      kMaxWorkers       = 48; 
constexpr size_t   kMaxReadBytes     = 2048;


IPListDialog::IPListDialog(wxWindow* parent, const wxString& hostname, const std::vector<boost::asio::ip::address>& ips, size_t& selected_index)
    : wxDialog(parent, wxID_ANY, _(L("Multiple resolved IP addresses")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_list(new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxSIMPLE_BORDER))
    , m_selected_index (selected_index)
{
    const int em = GUI::wxGetApp().em_unit();
    m_list->SetMinSize(wxSize(40 * em, 30 * em));

    wxBoxSizer* vsizer = new wxBoxSizer(wxVERTICAL);

    auto* label = new wxStaticText(this, wxID_ANY, GUI::format_wxstr(_L("There are several IP addresses resolving to hostname %1%.\nPlease select one that should be used."), hostname));
    vsizer->Add(label, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, em);

    m_list->SetSingleStyle(wxLC_SINGLE_SEL);
    m_list->AppendColumn(_(L("Address")), wxLIST_FORMAT_LEFT, 40 * em);

    for (size_t i = 0; i < ips.size(); i++)
        m_list->InsertItem(i, boost::nowide::widen(ips[i].to_string()));

    m_list->Select(0);

    vsizer->Add(m_list, 1, wxEXPAND | wxALL, em);

    wxBoxSizer* button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->Add(new wxButton(this, wxID_OK, "OK"), 0, wxALL, em);
    button_sizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0, wxALL, em);

    vsizer->Add(button_sizer, 0, wxALIGN_CENTER);
    SetSizerAndFit(vsizer);

    GUI::wxGetApp().UpdateDlgDarkUI(this);
}

IPListDialog::~IPListDialog()
{
}

void IPListDialog::EndModal(int retCode)
{
    if (retCode == wxID_OK) {
        m_selected_index = (size_t)m_list->GetFirstSelected();
    }
    wxDialog::EndModal(retCode);
}

// Events
class BonjourReplyEvent : public wxEvent
{
public:
    BonjourReply reply;

    BonjourReplyEvent(wxEventType eventType, int winid, BonjourReply &&reply) :
        wxEvent(winid, eventType),
        reply(std::move(reply))
    {}

    wxEvent* Clone() const override { return new BonjourReplyEvent(*this); }
};

wxDEFINE_EVENT(EVT_BONJOUR_REPLY, Slic3r::BonjourReplyEvent);
wxDEFINE_EVENT(EVT_BONJOUR_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SCAN_HIT, wxCommandEvent);
wxDEFINE_EVENT(EVT_DISCOVERY_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOG_APPEND, wxCommandEvent);

// Helpers
class ReplySet : public std::set<BonjourReply> {};

struct LifetimeGuard {
    std::mutex     mutex;
    BonjourDialog* dialog;
    explicit LifetimeGuard(BonjourDialog* d) : dialog(d) {}
};

static inline wxString now_tag()
{
    auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    wchar_t buf[32];
    std::wcsftime(buf, 32, L"%H:%M:%S", &tm);
    return wxString(buf);
}

static std::string get_local_ipv4()
{
    try {
        boost::asio::io_context io;
        boost::asio::ip::udp::socket s(io);
        s.connect({ boost::asio::ip::make_address("8.8.8.8"), 53 });
        return s.local_endpoint().address().to_string();
    } catch (...) {}
    return "127.0.0.1";
}

static bool tcp_connect_with_timeout(const std::string& ip, uint16_t port, int timeout_ms)
{
    using boost::asio::ip::tcp;
    try {
        boost::asio::io_context io;
        tcp::socket sock(io);
        sock.open(tcp::v4());
        sock.non_blocking(true);

        boost::system::error_code ec;
        sock.connect({ boost::asio::ip::make_address(ip), port }, ec);
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::in_progress) {
#ifdef _WIN32
            int fd = static_cast<int>(sock.native_handle());
#else
            int fd = sock.native_handle();
#endif
            fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
            timeval tv{ timeout_ms/1000, (timeout_ms%1000)*1000 };
            if (select(fd+1, nullptr, &wfds, nullptr, &tv) <= 0) return false;
            int soerr = 0; socklen_t len = sizeof(soerr);
#ifdef _WIN32
            getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
#else
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
#endif
            if (soerr) return false;
        } else if (ec) {
            return false;
        }
        return true;
    } catch (...) { return false; }
}

static bool looks_like_moonraker_http(boost::asio::ip::tcp::socket& sock,
                                      const std::string& ip, uint16_t port,
                                      const char* path, int timeout_ms)
{
    using boost::asio::buffer;
    try {
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << ip << ":" << port << "\r\n"
            << "User-Agent: slic3r-scan\r\n"
            << "Connection: close\r\n\r\n";
        const std::string payload = req.str();

        boost::system::error_code wec;
        sock.write_some(buffer(payload), wec);
        if (wec) return false;

        sock.non_blocking(true);
        std::string buf; buf.reserve(kMaxReadBytes);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::array<char, 512> tmp{};
        for (;;) {
            boost::system::error_code rec;
            auto n = sock.read_some(buffer(tmp), rec);
            if (n > 0) {
                buf.append(tmp.data(), n);
                if (buf.size() >= kMaxReadBytes) break;
            }
            if (rec == boost::asio::error::would_block || rec == boost::asio::error::try_again) {
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rec) break;
        }
        std::string low = buf;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);

        return (low.find("moonraker") != std::string::npos) ||
               (low.find("\"klippy\"")   != std::string::npos) ||
               (low.find("\"server\"")   != std::string::npos);
    } catch (...) { return false; }
}

static bool probe_moonraker_host(const std::string& ip, uint16_t port)
{
    using boost::asio::ip::tcp;
    try {
        boost::asio::io_context io;
        tcp::socket sock(io);
        if (!tcp_connect_with_timeout(ip, port, kConnectTimeoutMs)) return false;

        if (looks_like_moonraker_http(sock, ip, port, "/server/info",  kHttpTimeoutMs)) return true;
        sock = tcp::socket(io);
        if (!tcp_connect_with_timeout(ip, port, kConnectTimeoutMs)) return false;
        if (looks_like_moonraker_http(sock, ip, port, "/printer/info", kHttpTimeoutMs)) return true;
        sock = tcp::socket(io);
        if (!tcp_connect_with_timeout(ip, port, kConnectTimeoutMs)) return false;
        if (looks_like_moonraker_http(sock, ip, port, "/",            kHttpTimeoutMs)) return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[probe_moonraker_host] Exception at %s:%u — %s\n",
                ip.c_str(), port, e.what());
    }
    catch (...) {
        fprintf(stderr, "[probe_moonraker_host] Unknown non-std exception at %s:%u\n",
                ip.c_str(), port);
    }
    return false;
}

BonjourDialog::BonjourDialog(wxWindow *parent, Slic3r::PrinterTechnology tech)
    : wxDialog(parent, wxID_ANY, _(L("Network lookup")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER)
    , list(new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT|wxSIMPLE_BORDER))
    , replies(new ReplySet)
    , label(new wxStaticText(this, wxID_ANY, ""))
    , m_log(nullptr)
    , timer(new wxTimer())
    , timer_state(0)
    , tech(tech)
{
    const int em = GUI::wxGetApp().em_unit();
    list->SetMinSize(wxSize(80 * em, 30 * em));

    wxBoxSizer *vsizer = new wxBoxSizer(wxVERTICAL);

    vsizer->Add(label, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, em);

    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                           wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP);
    m_log->SetMinSize(wxSize(-1, 8 * em));
    vsizer->Add(m_log, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    list->SetSingleStyle(wxLC_SINGLE_SEL);
    list->SetSingleStyle(wxLC_SORT_DESCENDING);
    list->AppendColumn(_(L("Address")),      wxLIST_FORMAT_LEFT, 5 * em);
    list->AppendColumn(_(L("Hostname")),     wxLIST_FORMAT_LEFT, 10 * em);
    list->AppendColumn(_(L("Service name")), wxLIST_FORMAT_LEFT, 20 * em);
    if (tech == ptFFF) {
        list->AppendColumn(_(L("Version")), wxLIST_FORMAT_LEFT, 5 * em);
    }

    vsizer->Add(list, 1, wxEXPAND | wxALL, em);

    wxBoxSizer *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->Add(new wxButton(this, wxID_OK,     _L("OK")),     0, wxALL, em);
    button_sizer->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALL, em);
    vsizer->Add(button_sizer, 0, wxALIGN_CENTER);

    SetSizerAndFit(vsizer);

    Bind(EVT_BONJOUR_REPLY, &BonjourDialog::on_reply, this);

    // Only stop timer at the true end:
    Bind(EVT_BONJOUR_COMPLETE, [this](wxCommandEvent &) {
        this->timer_state = 0;
        label->SetLabel(_L("Searching for devices: Finished."));
        if (m_log) m_log->AppendText(now_tag() + "  Discovery complete.\n");
    });

    // Phase update (Bonjour finished => starting scan)
    Bind(EVT_DISCOVERY_PROGRESS, [this](wxCommandEvent &e) {
        if (e.GetInt() == 1) {
            label->SetLabel(_L("Searching for devices… (Moonraker scan)"));
            if (m_log) m_log->AppendText(now_tag() + "  Bonjour finished. Starting Moonraker scan…\n");
        }
    });

    // Append log lines
    Bind(EVT_LOG_APPEND, [this](wxCommandEvent &e) {
        if (m_log) m_log->AppendText(e.GetString() + "\n");
    });

    Bind(wxEVT_TIMER, &BonjourDialog::on_timer, this);
    Bind(EVT_SCAN_HIT, &BonjourDialog::on_scan_hit, this);

    GUI::wxGetApp().UpdateDlgDarkUI(this);
}

BonjourDialog::~BonjourDialog() {}

bool BonjourDialog::show_and_lookup()
{
    Show();

    timer->Stop();
    timer->SetOwner(this);
    timer_state = 1;
    timer->Start(1000);
    on_timer_process();

    auto dguard = std::make_shared<LifetimeGuard>(this);

    Bonjour::TxtKeys txt_keys { "version", "model" };

    if (m_log) m_log->AppendText(now_tag() + "  Bonjour: browsing for _moonraker._tcp …\n");

    bonjour = Bonjour("moonraker")
        .set_txt_keys(std::move(txt_keys))
        .set_retries(3)
        .set_timeout(5)
        .on_reply([dguard](BonjourReply &&reply) {
            std::lock_guard<std::mutex> lock_guard(dguard->mutex);
            if (auto *dialog = dguard->dialog) {
                auto evt = new BonjourReplyEvent(EVT_BONJOUR_REPLY, dialog->GetId(), std::move(reply));
                wxQueueEvent(dialog, evt);
            }
        })
        .on_complete([dguard]() {
            {   // phase: Bonjour finished (do NOT stop timer)
                std::lock_guard<std::mutex> lock_guard(dguard->mutex);
                if (auto *dialog = dguard->dialog) {
                    auto *evt = new wxCommandEvent(EVT_DISCOVERY_PROGRESS, dialog->GetId());
                    evt->SetInt(1); // “Bonjour done → starting scan”
                    wxQueueEvent(dialog, evt);
                }
            }
            {   // start fallback scan
                std::lock_guard<std::mutex> lock_guard(dguard->mutex);
                if (auto *dialog = dguard->dialog) {
                    dialog->start_moonraker_scan(dguard);
                }
            }
        })
        .lookup();

    bool res = ShowModal() == wxID_OK && list->GetFirstSelected() >= 0;
    {
        std::lock_guard<std::mutex> lock_guard(dguard->mutex);
        dguard->dialog = nullptr; // tell workers the dialog is gone
    }
    return res;
}

wxString BonjourDialog::get_selected() const
{
    auto sel = list->GetFirstSelected();
    return sel >= 0 ? list->GetItemText(sel) : wxString();
}

// Replies

void BonjourDialog::on_reply(BonjourReplyEvent &e)
{
    if (replies->find(e.reply) != replies->end()) {
        return; // de-dupe
    }

    //const auto model = e.reply.txt_data.find("model");
    //const bool sl1 = model != e.reply.txt_data.end() && model->second == "SL1";
    //if ((tech == ptFFF && sl1) || (tech == ptSLA && !sl1)) return;

    replies->insert(std::move(e.reply));

    auto selected = get_selected();

    wxWindowUpdateLocker freeze_guard(this);
    list->DeleteAllItems();

    for (const auto &reply : *replies) {
        auto item = list->InsertItem(0, reply.full_address);
        list->SetItem(item, 1, reply.hostname);
        list->SetItem(item, 2, reply.service_name);

        if (tech == ptFFF) {
            const auto it = reply.txt_data.find("version");
            if (it != reply.txt_data.end()) {
                list->SetItem(item, 3, GUI::from_u8(it->second));
            }
        }
    }

    const int em = GUI::wxGetApp().em_unit();
    for (int i = 0; i < list->GetColumnCount(); i++) {
        list->SetColumnWidth(i, wxLIST_AUTOSIZE);
        if (list->GetColumnWidth(i) < 10 * em) list->SetColumnWidth(i, 10 * em);
    }

    if (!selected.IsEmpty()) {
        auto hit = list->FindItem(-1, selected);
        if (hit >= 0) list->SetItemState(hit, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
}

// Timers

void BonjourDialog::on_timer(wxTimerEvent &) { on_timer_process(); }

void BonjourDialog::on_timer_process()
{
    const auto search_str = _L("Searching for devices");
    if (timer_state > 0) {
        const std::string dots(timer_state, '.');
        label->SetLabel(search_str + dots);
        timer_state = (timer_state) % 3 + 1;
    } else {
        label->SetLabel(search_str + ": " + _L("Finished") + ".");
        timer->Stop();
    }
}

void BonjourDialog::on_scan_hit(wxCommandEvent &e)
{
    const wxString addr = e.GetString();

    if (list->FindItem(-1, addr) >= 0) return;

    wxWindowUpdateLocker freeze_guard(this);

    long item = list->InsertItem(0, addr);
    list->SetItem(item, 1, _("(scanned)"));
    list->SetItem(item, 2, _("moonraker (scan)"));
    if (tech == ptFFF) list->SetItem(item, 3, _("scan"));

    const int em = GUI::wxGetApp().em_unit();
    for (int i = 0; i < list->GetColumnCount(); i++) {
        list->SetColumnWidth(i, wxLIST_AUTOSIZE);
        if (list->GetColumnWidth(i) < 10 * em) list->SetColumnWidth(i, 10 * em);
    }
}

// Fallback Scanner

void BonjourDialog::start_moonraker_scan(std::shared_ptr<LifetimeGuard> dguard)
{
    
    std::thread([dguard]() {
        // tell log we’re scanning and what subnet we inferred
        {
            std::lock_guard<std::mutex> lock(dguard->mutex);
            if (auto *dialog = dguard->dialog) {
                auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
                logevt->SetString(now_tag() + "  Scanning local /24 for Moonraker on port " +
                                  GUI::from_u8(std::to_string(kMoonrakerPort)) + " …");
                wxQueueEvent(dialog, logevt);
            }
        }

        const std::string local = get_local_ipv4();
        if (local.rfind("127.", 0) == 0) {
            std::lock_guard<std::mutex> lock(dguard->mutex);
            if (auto *dialog = dguard->dialog) {
                auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
                logevt->SetString(now_tag() + "  No usable local IP (127.0.0.1). Aborting scan.");
                wxQueueEvent(dialog, logevt);
                wxQueueEvent(dialog, new wxCommandEvent(EVT_BONJOUR_COMPLETE, dialog->GetId()));
            }
            return;
        }

        auto dot = local.find_last_of('.');
        if (dot == std::string::npos) {
            std::lock_guard<std::mutex> lock(dguard->mutex);
            if (auto *dialog = dguard->dialog) {
                auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
                logevt->SetString(now_tag() + "  Could not derive /24 from local IP: " + GUI::from_u8(local));
                wxQueueEvent(dialog, logevt);
                wxQueueEvent(dialog, new wxCommandEvent(EVT_BONJOUR_COMPLETE, dialog->GetId()));
            }
            return;
        }
        const std::string base = local.substr(0, dot);

        {
            std::lock_guard<std::mutex> lock(dguard->mutex);
            if (auto *dialog = dguard->dialog) {
                auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
                logevt->SetString(now_tag() + "  Subnet: " + GUI::from_u8(base) + ".0/24   Local: " + GUI::from_u8(local));
                wxQueueEvent(dialog, logevt);
            }
        }

        // split /24 across a small pool
        const int hosts = 254; // .1 .. .254
        const int chunk = std::max(1, hosts / kMaxWorkers);
        std::vector<std::thread> pool;
        
        const char* dbg = "10.0.0.164";   // e.g. 192.168.1.50
        const std::string debug_ip = dbg ? dbg : "";

        auto worker = [&](int start, int end) {
            for (int i = start; i <= end; ++i) {
                const std::string ip = base + "." + std::to_string(i);
                if (ip == local) continue;
                
                if (!debug_ip.empty() && ip != debug_ip) continue;

                if (probe_moonraker_host(ip, kMoonrakerPort)) {
                    std::lock_guard<std::mutex> lock(dguard->mutex);
                    if (auto *dialog = dguard->dialog) {
                        auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
                        logevt->SetString(now_tag() + "  Found Moonraker at " + GUI::from_u8(ip) + ":" + GUI::from_u8(std::to_string(kMoonrakerPort)));
                        wxQueueEvent(dialog, logevt);

                        auto *evt = new wxCommandEvent(EVT_SCAN_HIT, dialog->GetId());
                        evt->SetString(GUI::from_u8(ip + ":" + std::to_string(kMoonrakerPort)));
                        wxQueueEvent(dialog, evt);
                    }
                }
            }
        };

        int begin = 1;
        for (int w = 0; w < kMaxWorkers && begin <= hosts; ++w) {
            int end = std::min(hosts, begin + chunk - 1);
            pool.emplace_back(worker, begin, end);
            begin = end + 1;
        }
        for (auto &t : pool) t.join();

        // final completion (stop spinner + “Discovery complete”)
        std::lock_guard<std::mutex> lock(dguard->mutex);
        if (auto *dialog = dguard->dialog) {
            auto* logevt = new wxCommandEvent(EVT_LOG_APPEND, dialog->GetId());
            logevt->SetString(now_tag() + "  Moonraker scan finished.");
            wxQueueEvent(dialog, logevt);

            wxQueueEvent(dialog, new wxCommandEvent(EVT_BONJOUR_COMPLETE, dialog->GetId()));
        }
    }).detach();
}

} // namespace Slic3r
