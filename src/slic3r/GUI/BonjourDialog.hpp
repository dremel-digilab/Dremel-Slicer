#ifndef slic3r_BonjourDialog_hpp_
#define slic3r_BonjourDialog_hpp_

#include <cstddef>
#include <memory>

#include <boost/asio/ip/address.hpp>

#include <wx/dialog.h>
#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"

class wxListView;
class wxStaticText;
class wxTimer;
class wxTimerEvent;
class wxTextCtrl; 
class address;

namespace Slic3r {

class Bonjour;
class BonjourReplyEvent;
class ReplySet;

struct LifetimeGuard;
wxDECLARE_EVENT(EVT_SCAN_HIT, wxCommandEvent);
wxDECLARE_EVENT(EVT_DISCOVERY_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(EVT_LOG_APPEND, wxCommandEvent);
wxDECLARE_EVENT(EVT_BONJOUR_REPLY, Slic3r::BonjourReplyEvent);

class BonjourDialog: public wxDialog
{
public:
	BonjourDialog(wxWindow *parent, Slic3r::PrinterTechnology);
	BonjourDialog(BonjourDialog &&) = delete;
	BonjourDialog(const BonjourDialog &) = delete;
	BonjourDialog &operator=(BonjourDialog &&) = delete;
	BonjourDialog &operator=(const BonjourDialog &) = delete;
	~BonjourDialog();

	bool show_and_lookup();
	wxString get_selected() const;
private:
	wxListView *list;
    wxTextCtrl* m_log;  // progress log box
	std::unique_ptr<ReplySet> replies;
	wxStaticText *label;
	std::shared_ptr<Bonjour> bonjour;
	std::unique_ptr<wxTimer> timer;
	unsigned timer_state;
	Slic3r::PrinterTechnology tech;

    // Starts the Moonraker fallback scan after Bonjour completes.
    void start_moonraker_scan(std::shared_ptr<LifetimeGuard> dguard);

	virtual void on_reply(BonjourReplyEvent &);
	void on_timer(wxTimerEvent &);
    void on_timer_process();
    
    void on_scan_hit(wxCommandEvent &e);
};

class IPListDialog : public wxDialog
{
public:
	IPListDialog(wxWindow* parent, const wxString& hostname, const std::vector<boost::asio::ip::address>& ips, size_t& selected_index);
	IPListDialog(IPListDialog&&) = delete;
	IPListDialog(const IPListDialog&) = delete;
	IPListDialog& operator=(IPListDialog&&) = delete;
	IPListDialog& operator=(const IPListDialog&) = delete;
	~IPListDialog();

	virtual void EndModal(int retCode) wxOVERRIDE;
private:
	wxListView*		m_list;
	size_t&			m_selected_index;
};

}

#endif
