#include "web_ui.h"
#include "datadir.h"
#include <unknwn.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <string>
#include <vector>
#include "message_diff.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static HWND owner;
static WebUiAction on_action;
static WebUiMediaProvider media_provider;
static ComPtr<ICoreWebView2Environment> environment;
static ComPtr<ICoreWebView2Controller> controller;
static ComPtr<ICoreWebView2> view;
static EventRegistrationToken message_token{};
static EventRegistrationToken navigation_token{};
static EventRegistrationToken permission_token{};
static EventRegistrationToken resource_token{};
static std::string pending_state;
static bool suspended;

static std::string utf8(const wchar_t *value) {
    if (!value) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (length < 1) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}
static void quoted(std::string &out, const wchar_t *value) {
    out += '"';
    for (unsigned char c : utf8(value)) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) {
            static const char digits[] = "0123456789abcdef";
            out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15];
        } else out += static_cast<char>(c);
    }
    out += '"';
}
static void field(std::string &out, const char *name, const wchar_t *value) {
    out += '"'; out += name; out += "\":"; quoted(out, value); out += ',';
}
static void number(std::string &out, const char *name, int value) {
    out += '"'; out += name; out += "\":"; out += std::to_string(value); out += ',';
}
extern "C" int web_ui_active(void) { return view != nullptr; }
extern "C" void web_ui_set_media_provider(WebUiMediaProvider provider) { media_provider=provider; }
extern "C" void web_ui_resize(HWND window) {
    if (controller) { RECT rect; GetClientRect(window, &rect); controller->put_Bounds(rect); }
}
static std::vector<SentMessage> sent_messages;
static void post_text(const std::string &text) {
    if (!view || suspended) return;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (length < 1) return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, wide.data(), length))
        view->PostWebMessageAsJson(wide.c_str());
}
static uint32_t revision_of(const WebUiMessage &m) {
    return m.media_id * 7u + static_cast<uint32_t>(m.media_pending + 2) * 0x10001u;
}
static void message_json(std::string &out, const WebUiMessage &m) {
    out += '{'; field(out, "text", m.text); field(out, "author", m.author);
    number(out, "uid", static_cast<int>(m.uid));
    number(out, "room", m.room); number(out, "remote", m.remote);
    number(out, "hour", m.hour); number(out, "minute", m.minute);
    out += "\"mediaId\":"; out += std::to_string(m.media_id); out += ',';
    number(out, "mediaKind", static_cast<int>(m.media_kind));
    number(out, "mediaPending", m.media_pending);
    out.back() = '}';
}
/* The page keeps the message list; only what changed since the last post crosses the bridge. */
static void update_messages(const WebUiState *state) {
    const size_t count = state->message_count;
    std::vector<SentMessage> next;
    next.reserve(count);
    for (size_t i = 0; i < count; ++i) next.push_back({state->messages[i].uid, revision_of(state->messages[i])});
    const MessageDiff plan = plan_message_diff(sent_messages, next);
    if (plan.kind == MessageDiff::NONE) return;
    std::string json;
    if (plan.kind == MessageDiff::RESET) {
        json = "{\"type\":\"messages\",\"reset\":[";
        for (size_t i = 0; i < count; ++i) { if (i) json += ','; message_json(json, state->messages[i]); }
        json += "]}";
    } else {
        std::string updates, appended;
        for (size_t index : plan.updated) { if (!updates.empty()) updates += ','; message_json(updates, state->messages[index]); }
        for (size_t i = plan.kept; i < count; ++i) { if (!appended.empty()) appended += ','; message_json(appended, state->messages[i]); }
        json = "{\"type\":\"messages\",\"drop\":" + std::to_string(plan.drop) + ",\"update\":[" + updates + "],\"append\":[" + appended + "]}";
    }
    sent_messages = std::move(next);
    post_text(json);
}
extern "C" void web_ui_update(const WebUiState *state) {
    if (!state) return;
    std::string json = "{\"type\":\"state\",";
    field(json, "peer", state->peer); field(json, "self", state->self);
    field(json, "connection", state->connection); field(json, "voice", state->voice);
    number(json, "room", state->room); number(json, "connected", state->connected);
    number(json, "connecting", state->connecting); number(json, "callRequested", state->call_requested);
    number(json, "callIncoming", state->call_incoming); number(json, "muted", state->muted);
    number(json, "screenEnabled", state->screen_enabled); number(json, "sharing", state->sharing);
    number(json, "remoteScreen", state->remote_screen); number(json, "testMode", state->test_mode);
    number(json, "internetEnabled", state->internet_enabled);
    number(json, "networkPhase", state->network_phase);
    number(json, "networkError", state->network_error);
    field(json, "friendsNotice", state->friends_notice);
    field(json, "identity", state->identity);
    field(json, "identityPublic", state->identity_public);
    field(json, "identityEnc", state->identity_enc);
    field(json, "networkToken", state->network_token);
    field(json, "inviteCode", state->invite_code);
    field(json, "networkNotice", state->network_notice);
    field(json, "updateStatus", state->update_status);
    number(json, "updateAvailable", state->update_available);
    number(json, "updateBusy", state->update_busy);
    number(json, "mediaBusy", state->media_busy);
    json += "\"friends\":[";
    for (size_t i = 0; i < state->friend_count; ++i) {
        if (i) json += ',';
        json += '{'; field(json, "name", state->friends[i].name);
        number(json, "online", state->friends[i].online);
        json.back() = '}';
    }
    json += "],\"audio\":{";
    if (state->audio) {
        const WebUiAudio &audio = *state->audio;
        number(json, "inputChoice", audio.input_choice); number(json, "outputChoice", audio.output_choice);
        number(json, "inputVolume", audio.input_volume); number(json, "outputVolume", audio.output_volume);
        number(json, "previewActive", audio.preview_active); number(json, "previewLevel", audio.preview_level);
        number(json, "locked", audio.locked); number(json, "sandbox", audio.sandbox);
        number(json, "echo", audio.echo); number(json, "noise", audio.noise); number(json, "agc", audio.agc);
        number(json, "gate", audio.gate); number(json, "cleanupAvailable", audio.cleanup_available);
        field(json, "notice", audio.notice);
        json += "\"inputs\":[";
        for (int i = 0; i < audio.input_count; ++i) { if (i) json += ','; quoted(json, audio.inputs[i]); }
        json += "],\"outputs\":[";
        for (int i = 0; i < audio.output_count; ++i) { if (i) json += ','; quoted(json, audio.outputs[i]); }
        json += "]";
    }
    json += "}}";
    if (json != pending_state) {
        pending_state = std::move(json);
        post_text(pending_state);
    }
    update_messages(state);
}
extern "C" void web_ui_forget_sent(void) { pending_state.clear(); sent_messages.clear(); }
static std::wstring widen(const char *utf8) {
    std::wstring wide;
    int length = utf8 ? MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0) : 0;
    if (length > 1) { wide.resize(static_cast<size_t>(length)); MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length); wide.pop_back(); }
    return wide;
}
extern "C" void web_ui_service_event(int slot, const char *event, int code, const char *utf8) {
    std::string json = "{\"type\":\"service\",";
    number(json, "slot", slot); number(json, "code", code);
    json += "\"event\":"; quoted(json, widen(event).c_str()); json += ",\"data\":";
    if (utf8) quoted(json, widen(utf8).c_str()); else json += "null";
    json += '}';
    post_text(json);
}
extern "C" void web_ui_signature(const char *id, const char *hex) {
    std::string json = "{\"type\":\"signature\",\"id\":";
    quoted(json, widen(id).c_str()); json += ",\"signature\":";
    if (hex) quoted(json, widen(hex).c_str()); else json += "null";
    json += '}';
    post_text(json);
}
extern "C" void web_ui_levels(int self, int peer) {
    std::string json = "{\"type\":\"levels\",";
    number(json, "self", self); number(json, "peer", peer);
    json.back() = '}';
    post_text(json);
}
/* The user's own stylesheet, read at each request so a reload picks up edits. Missing means empty. */
static std::string read_user_css() {
    std::string css;
    wchar_t path[MAX_PATH];
    if (FAILED(murmur_local_appdata(path)) || wcslen(path) + 24 >= MAX_PATH) return css;
    wcscat_s(path, MAX_PATH, L"\\Murmur\\user.css");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return css;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 262144) {
        css.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, css.data(), static_cast<DWORD>(css.size()), &read, nullptr) || read != css.size()) css.clear();
    }
    CloseHandle(file);
    return css;
}
extern "C" void web_ui_ecdh(const char *id, const char *hex) {
    std::string json = "{\"type\":\"ecdh\",\"id\":";
    quoted(json, widen(id).c_str()); json += ",\"secret\":";
    if (hex) quoted(json, widen(hex).c_str()); else json += "null";
    json += '}';
    post_text(json);
}
extern "C" void web_ui_screen_signal(const wchar_t *signal) {
    if (!view || !signal) return;
    std::string json = "{\"type\":\"screen-signal\",\"payload\":";
    quoted(json, signal);
    json += '}';
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.c_str(), -1, nullptr, 0);
    if (length < 1) return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.c_str(), -1, wide.data(), length))
        view->PostWebMessageAsJson(wide.c_str());
}
extern "C" void web_ui_init(HWND window, WebUiAction action) {
    owner = window; on_action = action;
    /* Engine flags measured with scripts/measure_performance.ps1: the network service runs in the
       browser process, and background features that a chat window never uses stay off.
       The GPU process is kept separate on purpose (video decode and encode for screen sharing). */
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--enable-features=NetworkServiceInProcess2 "
        L"--disable-features=msSmartScreenProtection,msEdgeSpotlight,AutofillServerCommunication,OptimizationHints,Translate "
        L"--disable-background-networking --disable-component-update");
    /* The profile lives in the user's data folder, so the program folder stays read-only. */
    wchar_t profile[MAX_PATH] = L"";
    bool have_profile = SUCCEEDED(murmur_local_appdata(profile)) &&
        wcslen(profile) + 24 < MAX_PATH && wcscat_s(profile, MAX_PATH, L"\\Murmur") == 0;
    if (have_profile) {
        CreateDirectoryW(profile, nullptr);
        have_profile = wcscat_s(profile, MAX_PATH, L"\\webview2") == 0;
        if (have_profile) CreateDirectoryW(profile, nullptr);
    }
    HRESULT started = CreateCoreWebView2EnvironmentWithOptions(nullptr, have_profile ? profile : nullptr, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment *created) -> HRESULT {
                if (FAILED(hr) || !created) { if (on_action) on_action(L"webview-unavailable", L""); return S_OK; }
                if (!owner) return S_OK;
                environment = created;
                created->CreateCoreWebView2Controller(owner,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller *created_controller) -> HRESULT {
                            if (FAILED(result) || !created_controller) { if (on_action) on_action(L"webview-unavailable", L""); return S_OK; }
                            if (!owner) return S_OK;
                            controller = created_controller;
                            if (FAILED(controller->get_CoreWebView2(&view)) || !view) return S_OK;
                            web_ui_resize(owner);
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(view->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                            }
                            view->AddWebResourceRequestedFilter(L"https://murmur.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                            view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                [](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
                                    ComPtr<ICoreWebView2WebResourceRequest> request;
                                    LPWSTR uri = nullptr;
                                    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&uri))) return S_OK;
                                    int id = 0;
                                    std::string user_css;
                                    const uint8_t *media_bytes = nullptr;
                                    size_t media_size = 0;
                                    const wchar_t *mime = L"text/plain; charset=utf-8";
                                    if (!wcscmp(uri, L"https://murmur.local/index.html")) { id = 4; mime = L"text/html; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/style.css")) { id = 5; mime = L"text/css; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/app.js")) { id = 6; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/server.js")) { id = 7; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/e2ee.js")) { id = 8; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/theme.js")) { id = 9; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/events.js")) { id = 10; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/voice.js")) { id = 11; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/sounds.js")) { id = 12; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!wcscmp(uri, L"https://murmur.local/onboarding.js")) { id = 13; mime = L"text/javascript; charset=utf-8"; }
                                    else if (!_wcsnicmp(uri, L"https://murmur.local/art/", 25)) { wchar_t *art_end = nullptr; long art = wcstol(uri + 25, &art_end, 10); if (art >= 1 && art <= 15 && art_end && !wcscmp(art_end, L".jpg")) { id = 19 + (int)art; mime = L"image/jpeg"; } }
                                    else if (!_wcsnicmp(uri, L"https://murmur.local/user.css", 29)) {
                                        user_css = read_user_css(); media_bytes = reinterpret_cast<const uint8_t *>(user_css.data());
                                        media_size = user_css.size(); mime = L"text/css; charset=utf-8";
                                    }
                                    else if (media_provider && !_wcsnicmp(uri,L"https://murmur.local/media/",27)) {
                                        wchar_t *end = nullptr;
                                        unsigned long key = wcstoul(uri+27,&end,10);
                                        if(key && end && !*end) media_provider(static_cast<uint32_t>(key),&media_bytes,&media_size,&mime);
                                    }
                                    CoTaskMemFree(uri);
                                    HRSRC resource = id ? FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), RT_RCDATA) : nullptr;
                                    HGLOBAL loaded = resource ? LoadResource(GetModuleHandleW(nullptr), resource) : nullptr;
                                    const BYTE *bytes = media_bytes ? media_bytes : loaded ? static_cast<const BYTE *>(LockResource(loaded)) : nullptr;
                                    DWORD length = media_bytes ? static_cast<DWORD>(media_size) : loaded ? SizeofResource(GetModuleHandleW(nullptr), resource) : 0;
                                    static const BYTE empty[] = "";
                                    ComPtr<IStream> stream;
                                    stream.Attach(SHCreateMemStream(bytes ? bytes : empty, length));
                                    if (!stream) return S_OK;
                                    std::wstring headers = L"Content-Type: "; headers += mime;
                                    headers += L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff";
                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    if (SUCCEEDED(environment->CreateWebResourceResponse(stream.Get(), bytes ? 200 : 404,
                                            bytes ? L"OK" : L"Not Found", headers.c_str(), &response)))
                                        args->put_Response(response.Get());
                                    return S_OK;
                                }).Get(), &resource_token);
                            /* The microphone is what voice channels are for, so the page that is part of this
                               application may use it without a prompt. Everything else keeps the default. */
                            view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                                    LPWSTR uri = nullptr;
                                    COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                                    if (SUCCEEDED(args->get_PermissionKind(&kind)) && kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE &&
                                        SUCCEEDED(args->get_Uri(&uri)) && uri && _wcsnicmp(uri, L"https://murmur.local/", 21) == 0)
                                        args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                    if (uri) CoTaskMemFree(uri);
                                    return S_OK;
                                }).Get(), &permission_token);
                            view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                    LPWSTR uri = nullptr;
                                    if (SUCCEEDED(args->get_Uri(&uri))) {
                                        if (_wcsnicmp(uri, L"https://murmur.local/", 21) != 0) args->put_Cancel(TRUE);
                                        CoTaskMemFree(uri);
                                    }
                                    return S_OK;
                                }).Get(), &navigation_token);
                            view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                    LPWSTR source = nullptr, raw = nullptr;
                                    if (FAILED(args->get_Source(&source))) return S_OK;
                                    bool trusted = _wcsnicmp(source, L"https://murmur.local/", 21) == 0;
                                    CoTaskMemFree(source);
                                    if (!trusted || FAILED(args->TryGetWebMessageAsString(&raw))) return S_OK;
                                    std::wstring input(raw); CoTaskMemFree(raw);
                                    if (input == L"ready") web_ui_forget_sent();
                                    static const wchar_t *prefixes[] = {
                                        L"send:",L"friend-create:",L"friend-add:",L"friend-code:",L"friend-connect:",
                                        L"friend-forget:",L"audio-input:",L"audio-output:",L"audio-in-volume:",
                                        L"audio-out-volume:",L"net-invite:",L"net-reply:",L"net-apply:",L"open-url:",L"screen-signal:",
                                        L"service-open:",L"service-send:",L"service-close:",L"identity-sign:",L"identity-ecdh:",L"unread:",L"keepalive:",L"audio-cleanup:"
                                    };
                                    const size_t limit = input.compare(0, 13, L"service-send:") == 0 ? 74000 : 13000;
                                    if (input.size() <= limit && on_action) {
                                        bool matched = false;
                                        for (const wchar_t *prefix : prefixes) {
                                            size_t length = wcslen(prefix);
                                            if (input.compare(0,length,prefix)==0) {
                                                std::wstring action(prefix,length-1);
                                                on_action(action.c_str(),input.c_str()+length);
                                                matched=true; break;
                                            }
                                        }
                                        if (!matched && input.size()<40) on_action(input.c_str(),L"");
                                    }
                                    return S_OK;
                                }).Get(), &message_token);
                            view->Navigate(L"https://murmur.local/index.html");
                            if (on_action) on_action(L"ready", L"");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(started) && on_action) on_action(L"webview-unavailable", L"");
}
/* A minimized window needs no rendering. Lower the memory target, and when nothing is live
   (no call), suspend the page entirely; restoring resumes it and the caller resends state. */
extern "C" void web_ui_set_background(int hidden, int allow_suspend) {
    if (!view || !controller) return;
    ComPtr<ICoreWebView2_19> memory;
    if (SUCCEEDED(view.As(&memory)))
        memory->put_MemoryUsageTargetLevel(hidden ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    ComPtr<ICoreWebView2_3> suspendable;
    if (FAILED(view.As(&suspendable))) return;
    if (hidden && allow_suspend) {
        controller->put_IsVisible(FALSE);
        suspendable->TrySuspend(nullptr);
        suspended = true;
    } else if (!hidden) {
        controller->put_IsVisible(TRUE);
        if (suspended) { suspendable->Resume(); suspended = false; }
        web_ui_forget_sent();
    } else if (suspended) {
        /* A call began while suspended. */
        controller->put_IsVisible(TRUE); suspendable->Resume(); suspended = false; web_ui_forget_sent();
    }
}
extern "C" int web_ui_suspended(void) { return suspended; }
extern "C" void web_ui_close(void) {
    if (view) { view->remove_WebMessageReceived(message_token); view->remove_NavigationStarting(navigation_token); view->remove_PermissionRequested(permission_token); view->remove_WebResourceRequested(resource_token); }
    view.Reset();
    suspended = false;
    if (controller) controller->Close();
    controller.Reset(); environment.Reset(); owner = nullptr; on_action = nullptr;
}
