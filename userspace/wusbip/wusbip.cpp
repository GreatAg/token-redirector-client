﻿/*
 * Copyright (C) 2023 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "wusbip.h"
#include "taskbaricon.h"
#include "persist.h"
#include "wxutils.h"
#include "utils.h"
#include "font.h"
#include "app.h"
#include "log.h"


#include <libusbip/remote.h>
#include <libusbip/persistent.h>
#include <libusbip/src/file_ver.h>

#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/aboutdlg.h>
#include <wx/busyinfo.h>
#include <wx/dataview.h>
#include <wx/config.h>
#include <wx/textdlg.h>
#include <wx/headerctrl.h>
#include <wx/clipbrd.h>
#include <wx/persist/dataview.h>

#include <format>
#include <set>

namespace
{

using namespace usbip;

auto &g_key_devices = L"/devices";
auto &g_key_url = L"url";
auto &g_persistent_mark = L"\u2713"; // CHECK MARK, 2714 HEAVY CHECK MARK

consteval auto get_saved_keys()
{
        using key_val = std::pair<const wchar_t* const, column_pos_t>;

        return std::to_array<key_val>({
                { L"busid", COL_BUSID },
                { L"speed", COL_SPEED },
                { L"vendor", COL_VENDOR },
                { L"product", COL_PRODUCT },
                { L"notes", COL_NOTES },
        });
}

consteval auto get_saved_flags()
{
        unsigned int flags{};

        for (auto [key, col]: get_saved_keys()) {
                flags |= mkflag(col);
        }

        return flags;
}

void set_saved_columns(_Inout_ device_columns &dc, _In_ const device_columns &saved)
{
        for (auto [key, col]: get_saved_keys()) {
                dc[col] = saved[col];
        }
}

/*
 * @see is_empty(_In_ const device_columns &dc)
 * @see is_empty(_In_ const imported_device &d)
 */
auto is_empty(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev) noexcept
{
        for (auto col: {COL_VENDOR, COL_PRODUCT}) {
                if (auto &s = tree.GetItemText(dev, col); s.empty()) {
                        return true;
                }
        }

        return false;
}

/*
 * For device_state.state 
 */
constexpr auto is_port_residual(_In_ state st)
{
        switch (st) {
        using enum state;
        case unplugged: // port > 0 if previous state was unplugging
        case connecting: // port is zero
        case connected: // port is zero
                return true;
        case plugged: // port > 0
        case disconnected: // port > 0 if previous state was plugged
        case unplugging: // port > 0
        default:
                return false;
        }
}

auto as_set(_In_ std::vector<device_columns> v)
{
        return std::set<device_columns>(
                std::make_move_iterator(v.begin()), 
                std::make_move_iterator(v.end()));
}

void log(_In_ const device_state &st)
{
        auto &d = st.device;
        auto &loc = d.location;

        auto s = std::format("{}:{}/{} {}, port {}, devid {:04x}, speed {}, vid {:02x}, pid {:02x}", 
                                loc.hostname, loc.service, loc.busid, vhci::get_state_str(st.state), 
                                d.port, d.devid, static_cast<int>(d.speed), d.vendor, d.product);

        wxLogVerbose(wxString::FromUTF8(s));
}

void log(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev, _In_ const wxString &prefix)
{
        auto server = tree.GetItemParent(dev);
        auto &url = tree.GetItemText(server);

        auto s = wxString::Format(
                        L"%s: %s/%s, port '%s', speed '%s', vendor '%s', product '%s', "
                        L"state '%s', saved state '%s', auto '%s', notes '%s'", 
                        prefix, url,
                        tree.GetItemText(dev, COL_BUSID),
                        tree.GetItemText(dev, COL_PORT),
                        tree.GetItemText(dev, COL_SPEED),
                        tree.GetItemText(dev, COL_VENDOR), 
                        tree.GetItemText(dev, COL_PRODUCT), 
                        tree.GetItemText(dev, COL_STATE), 
                        tree.GetItemText(dev, COL_SAVED_STATE), 
                        tree.GetItemText(dev, COL_PERSISTENT), 
                        tree.GetItemText(dev, COL_NOTES));

        wxLogVerbose(s);
}

auto update_from_saved(
        _Inout_ device_columns &dc, _In_ unsigned int flags,
        _In_ const std::set<device_location> &persistent, 
        _In_opt_ const std::set<device_columns> *saved = nullptr)
{
        constexpr auto saved_flags = get_saved_flags();

        static_assert(saved_flags & mkflag(COL_NOTES));
        static_assert(!(saved_flags & mkflag(COL_PERSISTENT)));

        if (!saved) {
                //
        } else if (auto i = saved->find(dc); i != saved->end()) {

                if (usbip::is_empty(dc)) {
                        set_saved_columns(dc, *i);
                        flags |= saved_flags;
                } else {
                        dc[COL_NOTES] = (*i)[COL_NOTES];

                        constexpr auto notes_flag = mkflag(COL_NOTES);
                        wxASSERT(!(flags & notes_flag));
                        flags |= notes_flag;
                }
        }

        if (auto loc = make_device_location(dc); persistent.contains(loc)) {
                dc[COL_PERSISTENT] = g_persistent_mark;

                constexpr auto pers_flag = mkflag(COL_PERSISTENT);
                wxASSERT(!(flags & pers_flag));

                flags |= pers_flag;
        }

        return flags;
}

auto is_server_or_empty(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem item)
{
        return  is_empty(tree, item) ||
                tree.GetItemParent(item) == tree.GetRootItem(); // server
}

auto get_devices(_In_ const wxTreeListCtrl &tree)
{
        wxTreeListItems v;

        for (auto item = tree.GetFirstItem(); item.IsOk(); item = tree.GetNextItem(item)) {
                if (!is_server_or_empty(tree, item)) {
                        v.push_back(item);
                }
        }

        return v;
}

auto get_selected_devices(_In_ const wxTreeListCtrl &tree)
{
        wxTreeListItems v;
        tree.GetSelections(v);

        auto pred = [&tree] (auto &item) { return is_server_or_empty(tree, item); };
        std::erase_if(v, pred);

        return v;
}

auto make_device_location(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem server, _In_ wxTreeListItem device)
{
        auto &url = tree.GetItemText(server);
        auto &busid = tree.GetItemText(device);

        return usbip::make_device_location(url, busid);
}

auto get_persistent()
{
        std::set<device_location> result;
        bool success{};
        
        if (auto lst = vhci::get_persistent(get_vhci().get(), success); !success) {
                auto err = GetLastError();
                wxLogError(_("Cannot load persistent info\nError %#lx\n%s"), err, GetLastErrorMsg(err));
        } else for (auto &loc: lst) {
                if (auto [i, inserted] = result.insert(std::move(loc)); !inserted) {
                        wxLogVerbose(_("%s: failed to insert %s:%s/%s"), 
                                        wxString::FromAscii(__func__), wxString::FromUTF8(i->hostname), 
                                        wxString::FromUTF8(i->service), wxString::FromUTF8(i->busid));
                }
        }

        return result;
}

auto get_saved()
{
        auto &cfg = *wxConfig::Get();

        auto path = cfg.GetPath();
        cfg.SetPath(g_key_devices);

        std::vector<device_columns> result;
        wxString name;
        long idx;

        for (auto ok = cfg.GetFirstGroup(name, idx); ok; cfg.SetPath(L".."), ok = cfg.GetNextGroup(name, idx)) {

                cfg.SetPath(name);

                device_columns dev;
                auto &url = get_url(dev);

                url = cfg.Read(g_key_url);
                if (url.empty()) {
                        continue;
                }

                for (auto [key, col] : get_saved_keys()) {
                        dev[col] = cfg.Read(key);
                }

                if (dev[COL_BUSID].empty()) {
                        continue;
                }

                result.push_back(std::move(dev));
        }

        cfg.SetPath(path);
        return result;
}

auto to_string(_In_ _In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev)
{
        wxASSERT(tree.GetColumnCount() > COL_LAST_VISIBLE);

        wxString s;
        for (unsigned int n = COL_LAST_VISIBLE, i = 0; i <= n; ++i) {
                if (i) {
                        s += L',';
                } else if (auto server = tree.GetItemParent(dev); server.IsOk()) {
                        s += tree.GetItemText(server);
                        s += L'/';
                } else {
                        wxFAIL_MSG("server.IsOk");
                }
                s += tree.GetItemText(dev, i);
        }
        return s;
}

auto get_servers(_In_ const std::vector<device_columns> &devices)
{
        std::set<wxString> servers;

        for (wxString hostname, service; auto &dc: devices) {
                if (auto url = get_url(dc); split_server_url(url, hostname, service)) {
                        servers.insert(std::move(hostname));
                }
        }

        return servers;
}

} // namespace


class DeviceStateEvent : public wxEvent
{
public:
        DeviceStateEvent(_In_ device_state st) : 
                wxEvent(0, EVT_DEVICE_STATE),
                m_state(std::move(st)) {}

        wxEvent *Clone() const override { return new DeviceStateEvent(*this); }

        auto& get() const noexcept { return m_state; }
        auto& get() noexcept { return m_state; }

private:
        device_state m_state;
};
wxDEFINE_EVENT(EVT_DEVICE_STATE, DeviceStateEvent);


MainFrame::MainFrame(_In_ Handle read) : 
        Frame(nullptr),
        m_read(std::move(read)),
        m_log(new LogWindow(this, 
                m_menu_log->FindItem(ID_TOGGLE_LOG_WINDOW),
                m_menu_view->FindItem(wxID_ZOOM_IN),
                m_menu_view->FindItem(wxID_ZOOM_OUT),
                m_menu_view->FindItem(wxID_ZOOM_100)))
{
        wxASSERT(m_read);

        init();
        restore_state();
        post_refresh();
}

void MainFrame::init()
{
        m_log->SetVerbose(true); // produce messages for wxLOG_Info
        m_log->SetLogLevel(DEFAULT_LOGLEVEL);

        auto app_name = wxGetApp().GetAppDisplayName();
        SetTitle(app_name);

        SetIcon(wxIcon(L"USBip")); // see wxwidgets.rc
        
        if (auto cfg = wxConfig::Get()) {
                cfg->SetStyle(wxCONFIG_USE_LOCAL_FILE);
        }

        set_menu_columns_labels();
        m_comboBoxServer->SetMaxLength(NI_MAXHOST);

        auto port = get_tcp_port();
        m_spinCtrlPort->SetValue(wxString::FromAscii(port)); // NI_MAXSERV

        init_tree_list();

        Bind(EVT_DEVICE_STATE, &MainFrame::on_device_state, this);
}

/*
 * FIXME: set first column bold.
 * FIXME: wxTreeListCtrl::OnMouseWheel event does not work if it is set in wxFormBuilder
 */
void MainFrame::init_tree_list()
{
        auto &tree = *m_treeListCtrl;
        auto &dv = *tree.GetDataView();

        tree.GetView()->Bind(wxEVT_MOUSEWHEEL, wxMouseEventHandler(MainFrame::on_tree_mouse_wheel), this);

        if (auto hdr = dv.GenericGetHeader()) {
                hdr->Bind(wxEVT_MOUSEWHEEL, wxMouseEventHandler(MainFrame::on_tree_mouse_wheel), this);
        }

        if (auto colour = wxTheColourDatabase->Find(L"MEDIUM GOLDENROD"); colour.IsOk()) { // "WHEAT"
                dv.SetAlternateRowColour(colour);
        }
}

void MainFrame::restore_state()
{
        wxPersistentRegisterAndRestore(this, L"MainFrame"); // @see persist.h
        wxPersistentRegisterAndRestore(m_treeListCtrl->GetDataView(), m_treeListCtrl->GetName());
}

void MainFrame::post_refresh()
{
        wxCommandEvent evt(wxEVT_COMMAND_MENU_SELECTED, wxID_REFRESH);
        wxASSERT(m_menu_devices->FindItem(wxID_REFRESH)); // command belongs to this menu
        wxPostEvent(m_menu_devices, evt);
}

void MainFrame::post_exit()
{
        wxCommandEvent evt(wxEVT_COMMAND_MENU_SELECTED, wxID_EXIT);
        wxPostEvent(m_menu_file, evt);
}

void MainFrame::on_close(wxCloseEvent &event)
{
        wxLogVerbose(wxString::FromAscii(__func__));

        if (m_close_to_tray && event.CanVeto()) {
                event.Veto();
                iconize_to_tray();
                return;
        }

        break_read_loop();
        m_read_thread.join();

        Frame::on_close(event);
}

void MainFrame::on_exit(wxCommandEvent&)
{
        Close(true);
}

void MainFrame::iconize_to_tray()
{
        if (m_taskbar_icon) {
                wxASSERT(!m_taskbar_icon->IsIconInstalled());
        } else {
                m_taskbar_icon = std::make_unique<TaskBarIcon>();
        }

        if (!m_taskbar_icon->SetIcon(GetIcon(), GetTitle())) {
                wxLogError(_("Could not set taskbar icon"));
        } else if (IsShown()) {
                [[maybe_unused]] auto ok = Hide();
                wxASSERT(ok);
        }
}

void MainFrame::read_loop()
{
        auto on_exit = [] (auto frame)
        {
                std::lock_guard<std::mutex> lock(frame->m_read_close_mtx);
                frame->m_read.close();
        };

        std::unique_ptr<MainFrame, decltype(on_exit)> ptr(this, on_exit);

        for (device_state st; vhci::read_device_state(m_read.get(), st); ) {
                auto evt = new DeviceStateEvent(std::move(st));
                QueueEvent(evt); // see on_device_state()
        }

        if (auto err = GetLastError(); err != ERROR_OPERATION_ABORTED) { // see CancelSynchronousIo
                wxLogError(_("vhci::read_device_state error %#lx\n%s"), err, GetLastErrorMsg(err));
        }
}

void MainFrame::break_read_loop()
{
        auto cancel_read = [this] // CancelSynchronousIo hangs if thread was terminated
        {
                std::lock_guard<std::mutex> lock(m_read_close_mtx);
                return !m_read || CancelSynchronousIo(m_read_thread.native_handle());
        };

        for (int i = 0; i < 300 && !cancel_read(); ++i, std::this_thread::sleep_for(std::chrono::milliseconds(100))) {
                if (auto err = GetLastError(); err != ERROR_NOT_FOUND) { // cannot find a request to cancel
                        wxLogError(L"CancelSynchronousIo error %lu\n%s", err, wxSysErrorMsg(err));
                        break; // wxLogSysError does not compile if wxNO_IMPLICIT_WXSTRING_ENCODING is set
                }
        }
}

void MainFrame::on_device_state(_In_ DeviceStateEvent &event)
{
        auto &tree = *m_treeListCtrl;

        auto &st = event.get();
        auto st_empty = is_empty(st.device);

        log(st);
        
        if (can_show_balloon()) {
                cancel_balloon();
                auto s = _(vhci::get_state_str(st.state)) + L' ' + make_device_url(st.device.location);
                show_balloon(s);
        }

        auto [dev, added] = find_or_add_device(st.device.location);

        if (added) {
                auto server = tree.GetItemParent(dev);
                auto &url = tree.GetItemText(server);
                auto &busid = tree.GetItemText(dev);
                wxLogVerbose(_("Added %s/%s"), url, busid);
        } else {
                log(tree, dev, _("Before"));
        }

        if (st.state == state::connecting) {
                auto state = tree.GetItemText(dev, COL_STATE); // can be empty
                tree.SetItemText(dev, COL_SAVED_STATE, state);
                wxLogVerbose(_("Current state '%s' saved"), state);
        }

        if (!(st_empty && st.state == state::disconnected)) { // connection has failed/closed
                //
        } else if (auto saved_state = tree.GetItemText(dev, COL_SAVED_STATE); saved_state.empty()) {
                wxLogVerbose(_("Transient device removed"));
                remove_device(dev);
                return;
        } else {
                tree.SetItemText(dev, COL_STATE, saved_state);
                wxLogVerbose(_("Saved state '%s' restored"), saved_state);
                return;
        }

        auto [dc, flags] = make_device_columns(st);
        wxASSERT(flags);

        if (auto &port = dc[COL_PORT]; !port.empty() && is_port_residual(st.state)) {
                port.clear();
                flags |= mkflag(COL_PORT);
        }

        if (added || st_empty) {
                auto persistent = get_persistent();
                auto saved = as_set(get_saved());
                flags = update_from_saved(dc, flags, persistent, &saved);
        }

        update_device(dev, dc, flags);
        log(tree, dev, _("After"));
}

void MainFrame::on_has_devices_update_ui(wxUpdateUIEvent &event)
{
        auto v = get_devices(*m_treeListCtrl);
        event.Enable(!v.empty());
}

void MainFrame::on_has_selected_devices_update_ui(wxUpdateUIEvent &event)
{
        auto v = get_selected_devices(*m_treeListCtrl);
        event.Enable(!v.empty());
}

void MainFrame::on_copy_rows(wxCommandEvent&)
{
        wxString rows;
        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree)) {
                rows += to_string(tree, dev) + L'\n';
        }

        wxLogVerbose(rows);

        if (wxClipboardLocker lck; !lck) {
                wxLogError(_("Cannot lock the clipboard"));
        } else if (auto data = std::make_unique<wxTextDataObject>(rows); wxTheClipboard->SetData(data.get())) {
                data.release();
        } else {
                wxLogError(_("Cannot pass data to the clipboard"));
        }
}

void MainFrame::on_select_all(wxCommandEvent&)
{
        m_treeListCtrl->SelectAll();
}

wxTreeListItem MainFrame::get_edit_notes_device()
{
        auto &tree = *m_treeListCtrl;
        wxTreeListItem item;

        if (auto v = get_selected_devices(tree); v.size() == 1) {
                item = v.front();
        }

        return item;
}

void MainFrame::on_edit_notes_update_ui(wxUpdateUIEvent &event)
{
        auto item = get_edit_notes_device();
        event.Enable(item.IsOk());
}

void MainFrame::on_edit_notes(wxCommandEvent&)
{
        auto dev = get_edit_notes_device();
        if (!dev.IsOk()) {
                return;
        }

        auto &tree = *m_treeListCtrl;
        auto server = tree.GetItemParent(dev);

        auto url = tree.GetItemText(server);
        auto busid = tree.GetItemText(dev);
        auto caption = wxString::Format(_("Notes for %s/%s"), url, busid);

        auto vendor = tree.GetItemText(dev, COL_VENDOR);
        auto product = tree.GetItemText(dev, COL_PRODUCT);
        auto message = wxString::Format(L"%s\n%s", vendor, product);

        auto notes = tree.GetItemText(dev, COL_NOTES);

        wxTextEntryDialog dlg(this, message, caption, notes, wxTextEntryDialogStyle);
        dlg.SetMaxLength(256);

        if (dlg.ShowModal() == wxID_OK) {
                notes = dlg.GetValue();
                tree.SetItemText(dev, COL_NOTES, notes);
        }
}

bool MainFrame::is_persistent(_In_ wxTreeListItem device)
{
        auto &tree = *m_treeListCtrl;
   
        wxASSERT(tree.GetItemParent(device).IsOk()); // server
        wxASSERT(!tree.GetFirstChild(device).IsOk());

        auto &s = tree.GetItemText(device, COL_PERSISTENT);
        return !s.empty();
}

void MainFrame::set_persistent(_In_ wxTreeListItem device, _In_ bool persistent)
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(tree.GetItemParent(device).IsOk()); // server
        wxASSERT(!tree.GetFirstChild(device).IsOk());

        wxString val;
        if (persistent) {
                val = g_persistent_mark; // CHECK MARK, 2714 HEAVY CHECK MARK
        }

        tree.SetItemText(device, COL_PERSISTENT, val);
}

void MainFrame::on_log_show_update_ui(wxUpdateUIEvent &event)
{
        auto f = m_log->GetFrame();
        event.Check(f->IsVisible());
}

void MainFrame::on_log_show(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_log->Show(checked);
}

void MainFrame::on_view_zebra_update_ui(wxUpdateUIEvent &event)
{
        auto &dv = *m_treeListCtrl->GetDataView();
        auto check = dv.HasFlag(wxDV_ROW_LINES);
        event.Check(check);
}

void MainFrame::on_view_zebra(wxCommandEvent&)
{
        auto &dv = *m_treeListCtrl->GetDataView();
        dv.ToggleWindowStyle(wxDV_ROW_LINES);
        dv.Refresh(false);
}

void MainFrame::on_log_verbose_update_ui(wxUpdateUIEvent &event)
{
        auto verbose = m_log->GetLogLevel() == VERBOSE_LOGLEVEL;
        event.Check(verbose);
}

void MainFrame::on_log_verbose(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_log->SetLogLevel(checked ? VERBOSE_LOGLEVEL : DEFAULT_LOGLEVEL);
}

void MainFrame::on_start_in_tray_update_ui(wxUpdateUIEvent &event)
{
        event.Check(m_start_in_tray);
}

void MainFrame::on_start_in_tray(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_start_in_tray = checked;
}

void MainFrame::on_close_to_tray_update_ui(wxUpdateUIEvent &event)
{
        event.Check(m_close_to_tray);
}

void MainFrame::on_close_to_tray(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_close_to_tray = checked;
}

bool MainFrame::attach(_In_ const wxString &url, _In_ const wxString &busid)
{
        wxString hostname;
        wxString service;

        if (!split_server_url(url, hostname, service)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
        }

        device_location loc {
                .hostname = hostname.ToStdString(wxConvUTF8),
                .service = service.ToStdString(wxConvUTF8),
                .busid = busid.ToStdString(wxConvUTF8),
        };

        wxWindowDisabler dis;
        wxBusyInfo wait(wxString::Format(_("Attaching %s/%s"), url, busid), this);

        auto &vhci = get_vhci(); 
        return vhci::attach(vhci.get(), loc);
}

void MainFrame::on_attach(wxCommandEvent&)
{
        wxLogVerbose(wxString::FromAscii(__func__));

        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree)) {

                auto server = tree.GetItemParent(dev);
                auto url = tree.GetItemText(server);
                auto busid = tree.GetItemText(dev);

                if (!attach(url,  busid)) {
                        auto err = GetLastError();
                        wxLogError(_("Cannot attach %s/%s\nError %#lx\n%s"), url, busid, err, GetLastErrorMsg(err));
                }
        }
}

void MainFrame::on_detach(wxCommandEvent&)
{
        wxLogVerbose(wxString::FromAscii(__func__));
        
        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree)) {

                auto port = get_port(dev);
                if (!port) {
                        continue;
                }

                if (auto &vhci = get_vhci(); !vhci::detach(vhci.get(),  port)) {
                        auto err = GetLastError();

                        auto server = tree.GetItemParent(dev);
                        auto url = tree.GetItemText(server);
                        auto busid = tree.GetItemText(dev);

                        wxLogError(_("Cannot detach %s/%s\nError %#lx\n%s"), url, busid, err, GetLastErrorMsg(err));
                }
        }
}

void MainFrame::on_detach_all(wxCommandEvent&) 
{
        wxLogVerbose(wxString::FromAscii(__func__));

        if (auto &vhci = get_vhci(); !vhci::detach(vhci.get(), -1)) {
                auto err = GetLastError();
                wxLogError(_("Cannot detach all devices\nError %#lx\n%s"), err, GetLastErrorMsg(err));
        }
}

wxTreeListItem MainFrame::find_or_add_server(_In_ const wxString &url)
{
        auto &tree = *m_treeListCtrl;
        wxTreeListItem server;

        for (auto item = tree.GetFirstItem(); item.IsOk(); item = tree.GetNextSibling(item)) {
                if (tree.GetItemText(item) == url) {
                        return server = item;
                }
        }

        return server = tree.AppendItem(tree.GetRootItem(), url);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const wxString &url, _In_ const wxString &busid)
{
        std::pair<wxTreeListItem, bool> res;

        auto &tree = *m_treeListCtrl;
        auto server = find_or_add_server(url);

        for (auto item = tree.GetFirstChild(server); item.IsOk(); item = tree.GetNextSibling(item)) {
                if (tree.GetItemText(item) == busid) {
                        return res = std::make_pair(item, false);
                }
        }

        auto device = tree.AppendItem(server, busid);

        if (!tree.IsExpanded(server)) {
                tree.Expand(server);
        }

        return res = std::make_pair(device, true);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const device_location &loc)
{
        auto url = make_server_url(loc);
        auto busid = wxString::FromUTF8(loc.busid);

        return find_or_add_device(url, busid);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const device_columns &dc)
{
        auto &url = get_url(dc);
        return find_or_add_device(url, dc[COL_BUSID]);
}

void MainFrame::remove_device(_In_ wxTreeListItem device)
{
        wxASSERT(device.IsOk());
        auto &tree = *m_treeListCtrl;

        auto server = tree.GetItemParent(device);
        tree.DeleteItem(device);

        if (auto child = tree.GetFirstChild(server); !child.IsOk()) { // has no children
                tree.DeleteItem(server);
        }
}

void MainFrame::update_device(_In_ wxTreeListItem device, _In_ const device_columns &dc, _In_ unsigned int flags)
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(device.IsOk());
        wxASSERT(tree.GetItemText(device) == dc[COL_BUSID]);
        wxASSERT(tree.GetItemText(tree.GetItemParent(device)) == get_url(dc));

        if (!flags) {
                return;
        }
        
        for (auto col: { COL_PORT, COL_SPEED, COL_VENDOR, COL_PRODUCT, COL_STATE, COL_PERSISTENT, COL_NOTES }) {
                if (auto &new_val = dc[col]; 
                    (flags & mkflag(col)) && new_val != tree.GetItemText(device, col)) {
                        tree.SetItemText(device, col, new_val);
                }
        }
}

void MainFrame::on_help_about(wxCommandEvent&)
{
        auto &v = win::get_file_version();
 
        wxAboutDialogInfo d;

        d.SetVersion(wx_string(v.GetProductVersion()));
        d.SetDescription(wx_string(v.GetFileDescription()));
        d.SetCopyright(wx_string(v.GetLegalCopyright()));

        //d.AddDeveloper(L"Vadym Hrynchyshyn\t<vadimgrn@gmail.com>");
        d.SetWebSite(L"https://apk-group.net", _("APK Website"));

        //d.SetLicence(_("GNU General Public License v3.0"));
        //d.SetIcon();

        wxAboutBox(d, this);
}

void MainFrame::add_exported_devices(wxCommandEvent&)
{
        auto &cb = *m_comboBoxServer;

        auto host = cb.GetValue();
        if (host.empty()) {
                cb.SetFocus();
                return;
        }

        auto port = wxString::Format(L"%d", m_spinCtrlPort->GetValue());
        wxLogVerbose(L"%s, host='%s', port='%s'", wxString::FromAscii(__func__), host, port);

        auto u8_host = host.ToStdString(wxConvUTF8);
        auto u8_port = port.ToStdString(wxConvUTF8);

        Socket sock;
        {
                wxWindowDisabler dis;
                wxBusyInfo wait(wxString::Format(_("Connecting to %s:%s"), host, port), this);

                sock = connect(u8_host.c_str(), u8_port.c_str());
        }

        if (!sock) {
                auto err = GetLastError();
                wxLogError(_("Cannot connect to %s:%s\nError %#lx\n%s"), host, port, err, GetLastErrorMsg(err));
                return;
        }

        auto persistent = get_persistent();
        auto saved = as_set(get_saved());

        auto dev = [this, host = std::move(u8_host), port = std::move(u8_port), &persistent, &saved] (auto, auto &device)
        {
                device_state st {
                        .device = make_imported_device(std::move(host), std::move(port), device),
                        .state = state::unplugged
                };

                auto [dc, flags] = make_device_columns(st);
                flags = update_from_saved(dc, flags, persistent, &saved);

                auto [item, added] = find_or_add_device(dc);
                if (!added) {
                        flags &= ~mkflag(COL_STATE); // clear
                }

                update_device(item, dc, flags);
        };

        auto intf = [this] (auto /*dev_idx*/, auto& /*dev*/, auto /*idx*/, auto& /*intf*/) {};

        if (!enum_exportable_devices(sock.get(), dev, intf)) {
                auto err = GetLastError();
                wxLogError(_("enum_exportable_devices error %#lx\n%s"), err, GetLastErrorMsg(err));
        } else if (cb.FindString(host) != wxNOT_FOUND) {
                // already exists
        } else if (auto pos = cb.Append(host); cb.GetCount() > 32) {
                cb.Delete(pos > 0 ? --pos : ++pos);
        }
}

void MainFrame::set_menu_columns_labels()
{
        constexpr auto cnt = COL_LAST_VISIBLE + 1;

        auto &menu = *m_menu_columns;
        wxASSERT(menu.GetMenuItemCount() == cnt);

        auto &view = *m_treeListCtrl->GetDataView();
        wxASSERT(cnt < view.GetColumnCount());

        for (auto pos = 0; pos < cnt; ++pos) {
                auto col = view.GetColumn(pos);
                auto title = col->GetTitle();

                if (auto item = menu.FindItemByPosition(pos); item->GetItemLabel() != title) {
                        item->SetItemLabel(title);
                }
        }
}

wxDataViewColumn* MainFrame::find_column(_In_ const wxString &title) const noexcept
{
        auto &view = *m_treeListCtrl->GetDataView();

        for (auto n = view.GetColumnCount(), pos = 0U; pos < n; ++pos) {
                if (auto col = view.GetColumn(pos); col->GetTitle() == title) {
                        return col;
                }
        }

        wxLogDebug(_("%s: column '%s' not found"), wxString::FromAscii(__func__), title);
        return nullptr;
}

wxDataViewColumn* MainFrame::find_column(_In_ int item_id) const noexcept
{
        if (auto item = m_menu_columns->FindItem(item_id)) {
                auto title = item->GetItemLabel(); 
                return find_column(title);
        }

        wxLogDebug(_("%s: item id '%d' not found"), wxString::FromAscii(__func__), item_id);
        return nullptr;
}

void MainFrame::on_view_column_update_ui(wxUpdateUIEvent &event)
{
        if (auto id = event.GetId(); auto col = find_column(id)) {
                event.Check(col->IsShown());
        }
}

void MainFrame::on_view_column(wxCommandEvent &event)
{
        if (auto id = event.GetId(); auto col = find_column(id)) {
                bool checked = event.GetInt();
                col->SetHidden(!checked);
        }
}

void MainFrame::on_item_activated(wxTreeListEvent &event)
{
        auto &tree = *m_treeListCtrl;
        
        if (auto item = event.GetItem(); tree.GetItemParent(item) == tree.GetRootItem()) {
                // item is a server
        } else if (auto state = tree.GetItemText(item, COL_STATE);
                   state == _(vhci::get_state_str(state::unplugged))) {
                on_attach(event);
        } else if (state == _(vhci::get_state_str(state::plugged))) {
                on_detach(event);
        }
}

void MainFrame::on_view_labels_update_ui(wxUpdateUIEvent &event)
{
        auto shown = m_auiToolBar->HasFlag(wxAUI_TB_TEXT);
        event.Check(shown);
}

void MainFrame::on_view_labels(wxCommandEvent &)
{
        auto &tb = *m_auiToolBar;
        tb.ToggleWindowStyle(wxAUI_TB_TEXT);
        tb.Refresh(false);
}

int MainFrame::get_port(_In_ wxTreeListItem dev) const
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(!tree.GetFirstChild(dev).IsOk());
        auto str = tree.GetItemText(dev, COL_PORT);

        int port;
        return str.ToInt(&port) ? port : 0;
}

void MainFrame::on_toogle_auto(wxCommandEvent&)
{
        for (auto &dev: get_selected_devices(*m_treeListCtrl)) {
                auto ok = is_persistent(dev);
                set_persistent(dev, !ok);
        }
}

void MainFrame::save(_In_ const wxTreeListItems &devices)
{
        auto &cfg = *wxConfig::Get();
        auto path = cfg.GetPath();

        cfg.DeleteGroup(g_key_devices);
        cfg.SetPath(g_key_devices);

        std::vector<device_location> persistent;
        auto &tree = *m_treeListCtrl;

        for (int cnt = 0; auto &dev: devices) {

                cfg.SetPath(wxString::Format(L"%d", ++cnt));

                auto server = tree.GetItemParent(dev);
                auto url = tree.GetItemText(server);

                cfg.Write(g_key_url, url);

                for (auto [key, col] : get_saved_keys()) {
                        auto value = tree.GetItemText(dev, col);
                        cfg.Write(key, value);
                }

                if (is_persistent(dev)) {
                        persistent.emplace_back(make_device_location(tree, server, dev));
                }

                cfg.SetPath(L"..");
        }

        cfg.Flush();
        cfg.SetPath(path);

        wxLogStatus(_("%zu device(s) saved"), devices.size());

        if (!vhci::set_persistent(get_vhci().get(), persistent)) {
                auto err = GetLastError();
                wxLogError(_("Cannot save persistent info\nError %#lx\n%s"), err, GetLastErrorMsg(err));
        }
}

void MainFrame::on_save(wxCommandEvent&)
{
        auto devices = get_devices(*m_treeListCtrl);
        save(devices);
}

void MainFrame::on_save_selected(wxCommandEvent&)
{
        if (auto devices = get_selected_devices(*m_treeListCtrl); devices.empty()) {
                wxMessageBox(_("There are no selected devices"), _("Save selected"), wxICON_WARNING);
        } else {
                save(devices);
        }
}

void MainFrame::on_load(wxCommandEvent&)
{
        int cnt = 0;
        auto saved = get_saved();

        if (static bool once{}; !once) {
                once = true;
                for (auto &i: get_servers(saved)) {
                        m_comboBoxServer->Append(i);
                }
        }

        for (auto persistent = get_persistent(); auto &dc: saved) {

                auto flags = get_saved_flags();
                auto [dev, added] = find_or_add_device(dc);

                if (!(added || is_empty(*m_treeListCtrl, dev))) {
                        wxLogVerbose(_("Skip loading existing device %s/%s"), get_url(dc), dc[COL_BUSID]);
                        continue;
                }

                constexpr auto state_flag = mkflag(COL_STATE);
                static_assert(!(get_saved_flags() & state_flag));

                dc[COL_STATE] = _(vhci::get_state_str(state::unplugged));
                flags = update_from_saved(dc, flags | state_flag, persistent);

                update_device(dev, dc, flags);
                ++cnt;
        }

        wxLogStatus(_("%d device(s) loaded"), cnt);
}

void MainFrame::on_reload(wxCommandEvent &event)
{
        wxLogVerbose(wxString::FromAscii(__func__));

        auto &tree = *m_treeListCtrl;
        tree.DeleteAllItems();

        bool ok{};
        auto devices = vhci::get_imported_devices(get_vhci().get(), ok);
        if (!ok) {
                auto err = GetLastError();
                wxLogError(_("Cannot get imported devices\nError %#lx\n%s"), err, GetLastErrorMsg(err));
                return;
        }

        auto persistent = get_persistent();
        auto saved = as_set(get_saved());

        for (auto &dev: devices) {
                auto [item, added] = find_or_add_device(dev.location);
                wxASSERT(added);
                
                device_state st { 
                        .device = std::move(dev), 
                        .state = state::plugged 
                };

                auto [dc, flags] = make_device_columns(st);
                flags = update_from_saved(dc, flags, persistent, &saved);

                update_device(item, dc, flags);
        }

        if (static bool once; !once) {
                once = true;
                on_load(event);
        }
}

std::unique_ptr<wxMenu> MainFrame::create_menu(_In_ const menu_item_descr *items, _In_ int cnt)
{
        auto menu = std::make_unique<wxMenu>();

        for (int i = 0; i < cnt; ++i) {
                if (auto [id, src, handler] = items[i]; clone_menu_item(*menu, id, *src)) {
                        menu->Bind(wxEVT_COMMAND_MENU_SELECTED, handler, this, id);
                }
        }

        return menu;
}

std::unique_ptr<wxMenu> MainFrame::create_tree_popup_menu()
{
        const auto items = std::to_array<menu_item_descr>({
                { wxID_SELECTALL, m_menu_edit, &MainFrame::on_select_all },
                { wxID_COPY, m_menu_edit, &MainFrame::on_copy_rows },
                { wxID_SEPARATOR, nullptr, nullptr },
                { wxID_OPEN, m_menu_devices, &MainFrame::on_attach },
                { wxID_CLOSE, m_menu_devices, &MainFrame::on_detach },
                { wxID_SEPARATOR, nullptr, nullptr },
                { ID_TOGGLE_AUTO, m_menu_edit, &MainFrame::on_toogle_auto },
                { ID_EDIT_NOTES, m_menu_edit, &MainFrame::on_edit_notes },
                { wxID_SEPARATOR, nullptr, nullptr },
                { wxID_SAVEAS, m_menu_file, &MainFrame::on_save_selected },
        });

        return create_menu(items.data(), items.size());
}

void MainFrame::on_item_context_menu(wxTreeListEvent&)
{
        if (!m_tree_popup_menu) {
                m_tree_popup_menu = create_tree_popup_menu();
        }

        PopupMenu(m_tree_popup_menu.get());
}

/*
 * @see wxConfig::Get()->DeleteAll()
 */
void MainFrame::on_view_reset(wxCommandEvent&)
{
        wxMessageDialog dlg(this, _("Reset all settings and restart the app?"), _("Reset settings"),
                wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING | wxCENTRE);

        if (dlg.ShowModal() == wxID_CANCEL) {
                return;
        }

        wxPersistenceManager::Get().DisableSaving();	
        wxConfig::Get()->DeleteGroup(L"Persistent_Options"); // FIXME: private key, defined in src\msw\regconf.cpp

        wchar_t** argv = wxGetApp().argv;

        switch (wxExecute(argv)) {
        case 0: // the command could not be executed
        case -1: // can happen when using DDE under Windows for command execution
                wxLogError(_("Cannot relaunch itself, please restart the app"));
                break;
        default:
                post_exit();
        }
}

void MainFrame::on_help_about_lib(wxCommandEvent&)
{
        wxInfoMessageBox(this);
}

/*
 * FIXME: how to layout controls after font size change?
 * 
 * auto wnd = static_cast<wxWindow*>(event.GetEventObject());
 * change_font_size(wnd, event.GetWheelRotation());
 */
void MainFrame::on_frame_mouse_wheel(wxMouseEvent &event)
{
        on_tree_mouse_wheel(event);
}

void MainFrame::on_tree_mouse_wheel(_In_ wxMouseEvent &event)
{
        if (event.GetModifiers() == wxMOD_CONTROL) { // only Ctrl is depressed
                change_font_size(m_treeListCtrl, event.GetWheelRotation());
        }
}

void MainFrame::on_view_font_increase(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, 1);
}

void MainFrame::on_view_font_decrease(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, -1);
}

void MainFrame::on_view_font_default(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, 0);
}

bool MainFrame::can_show_balloon() const noexcept
{ 
        return m_taskbar_icon && m_taskbar_icon->IsIconInstalled(); 
}

void MainFrame::show_balloon(_In_ const wxString &text, _In_ int flags)
{
        wxASSERT(m_taskbar_icon);
        if (!m_taskbar_icon->ShowBalloon(wxEmptyString, text, 0, flags)) {
                wxLogError(_("Could not show balloon notification"));
        }
}
