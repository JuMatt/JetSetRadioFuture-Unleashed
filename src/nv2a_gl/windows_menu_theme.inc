/* Supported Win32 owner-draw menus and DWM caption colour. No undocumented
 * uxtheme ordinals or global theme changes. Only the native window chrome. */
typedef struct
{
    WCHAR text[160];
    int bar, popup, separator;
} JmMenuLabel;
static JmMenuLabel jm_menu_labels[48];
static int jm_menu_label_count, jm_dark, jm_high_contrast;
static COLORREF jm_bg, jm_fg, jm_selected, jm_disabled;
static HBRUSH jm_menu_brush;
static HFONT jm_menu_font;
static WNDPROC jm_original_wndproc;
static HMENU jm_audio_menu;
static void jm_open_mixer(void);

static int jm_system_dark(void)
{
    DWORD light = 1, size = sizeof light;
    HIGHCONTRASTW hc = {0};
    hc.cbSize = sizeof hc;
    const char *preview = getenv("JSRF_UI_THEME"); /* test process only */
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0);
    jm_high_contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    if (jm_high_contrast)
        return 0;
    if (preview && !strcmp(preview, "dark"))
        return 1;
    if (preview && !strcmp(preview, "light"))
        return 0;
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
}

static void jm_theme_refresh(void)
{
    NONCLIENTMETRICSW metrics = {0};
    metrics.cbSize = sizeof metrics;
    BOOL dark;
    typedef HRESULT(WINAPI * SetDwmAttribute)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm;
    static SetDwmAttribute set_dwm;
    jm_dark = jm_system_dark();
    jm_bg = jm_high_contrast ? GetSysColor(COLOR_MENU)
            : jm_dark        ? RGB(29, 32, 36)
                             : RGB(248, 249, 250);
    jm_fg = jm_high_contrast ? GetSysColor(COLOR_MENUTEXT)
            : jm_dark        ? RGB(239, 242, 245)
                             : RGB(26, 31, 36);
    jm_selected = jm_high_contrast ? GetSysColor(COLOR_HIGHLIGHT)
                  : jm_dark        ? RGB(61, 69, 76)
                                   : RGB(220, 234, 239);
    jm_disabled = GetSysColor(COLOR_GRAYTEXT);
    if (jm_menu_brush)
        DeleteObject(jm_menu_brush);
    jm_menu_brush = CreateSolidBrush(jm_bg);
    if (jm_menu_font)
        DeleteObject(jm_menu_font);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0))
        jm_menu_font = CreateFontIndirectW(&metrics.lfMenuFont);
    /* MIM_APPLYTOSUBMENUS gives every popup the same background. */
    if (g_menu_bar)
    {
        MENUINFO mi = {0};
        mi.cbSize = sizeof mi;
        mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        mi.hbrBack = jm_menu_brush;
        SetMenuInfo(g_menu_bar, &mi);
    }
    if (!dwm)
    {
        dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm)
            set_dwm = (SetDwmAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    dark = jm_dark;
    if (set_dwm && FAILED(set_dwm(g_menu_hwnd, 20, &dark, sizeof dark)))
        set_dwm(g_menu_hwnd, 19, &dark, sizeof dark);
    DrawMenuBar(g_menu_hwnd);
    RedrawWindow(g_menu_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
}

static void jm_theme_menu(HMENU menu, int bar)
{
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count && jm_menu_label_count < 48; i++)
    {
        MENUITEMINFOW mi = {0};
        mi.cbSize = sizeof mi;
        JmMenuLabel *label = &jm_menu_labels[jm_menu_label_count++];
        mi.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
        mi.dwTypeData = label->text;
        mi.cch = 159;
        GetMenuItemInfoW(menu, i, TRUE, &mi);
        label->bar = bar;
        label->popup = mi.hSubMenu != NULL;
        label->separator = (mi.fType & MFT_SEPARATOR) != 0;
        if (mi.hSubMenu)
            jm_theme_menu(mi.hSubMenu, 0);
        mi.fMask = MIIM_FTYPE | MIIM_DATA;
        mi.fType |= MFT_OWNERDRAW;
        mi.dwItemData = (ULONG_PTR)label;
        SetMenuItemInfoW(menu, i, TRUE, &mi);
    }
}

static LRESULT CALLBACK jm_menu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Handle the native key once, including injected/virtual-key keyboards.
     * Returning here keeps SDL from also creating a second open request. */
    if (msg == WM_KEYDOWN && wp == VK_F6)
    {
        if (!(lp & (1L << 30)))
            jm_open_mixer();
        return 0;
    }
    if (msg == WM_MENUCHAR)
    {
        HMENU menu = (HMENU)lp;
        WCHAR key = (WCHAR)LOWORD(wp);
        for (int i = 0; i < GetMenuItemCount(menu); i++)
        {
            MENUITEMINFOW mi = {0};
            mi.cbSize = sizeof mi;
            mi.fMask = MIIM_DATA | MIIM_STATE;
            if (!GetMenuItemInfoW(menu, i, TRUE, &mi) || !mi.dwItemData ||
                (mi.fState & MFS_DISABLED))
                continue;
            JmMenuLabel *label = (JmMenuLabel *)mi.dwItemData;
            WCHAR *mnemonic = wcschr(label->text, L'&');
            if (mnemonic && towupper(mnemonic[1]) == towupper(key))
                return MAKELRESULT(i, MNC_EXECUTE);
        }
    }
    if (msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED)
        jm_theme_refresh();
    if (msg == WM_MEASUREITEM && !wp)
    {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        JmMenuLabel *label = (JmMenuLabel *)m->itemData;
        if (m->CtlType == ODT_MENU && label)
        {
            HDC dc = GetDC(hwnd);
            HGDIOBJ old = SelectObject(dc, jm_menu_font);
            SIZE s;
            GetTextExtentPoint32W(dc, label->text, (int)wcslen(label->text), &s);
            m->itemWidth = label->bar ? s.cx + 12 : s.cx + 64;
            m->itemHeight = label->separator ? 9 : s.cy + 12;
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
            return TRUE;
        }
    }
    if (msg == WM_DRAWITEM && !wp)
    {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        JmMenuLabel *label = (JmMenuLabel *)d->itemData;
        if (d->CtlType == ODT_MENU && label)
        {
            RECT r = d->rcItem;
            int selected = d->itemState & (ODS_SELECTED | ODS_HOTLIGHT);
            HBRUSH brush = CreateSolidBrush(selected ? jm_selected : jm_bg);
            FillRect(d->hDC, &r, brush);
            DeleteObject(brush);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, (d->itemState & ODS_DISABLED)  ? jm_disabled
                                 : selected && jm_high_contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                                                : jm_fg);
            HGDIOBJ old = SelectObject(d->hDC, jm_menu_font);
            if (label->separator)
            {
                HPEN pen = CreatePen(PS_SOLID, 1, jm_disabled);
                HGDIOBJ op = SelectObject(d->hDC, pen);
                MoveToEx(d->hDC, r.left + 28, (r.top + r.bottom) / 2, NULL);
                LineTo(d->hDC, r.right - 10, (r.top + r.bottom) / 2);
                SelectObject(d->hDC, op);
                DeleteObject(pen);
            }
            else
            {
                WCHAR left[160];
                wcscpy(left, label->text);
                WCHAR *tab = wcschr(left, L'\t');
                if (tab)
                    *tab++ = 0;
                if (label->bar)
                    DrawTextW(d->hDC, left, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                else
                {
                    RECT text = r;
                    text.left += 28;
                    text.right -= 24;
                    DrawTextW(d->hDC, left, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    if (tab)
                        DrawTextW(d->hDC, tab, -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    RECT mark = r;
                    mark.right = mark.left + 25;
                    if (d->itemState & ODS_CHECKED)
                        DrawTextW(d->hDC, L"\x2713", 1, &mark,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    if (label->popup)
                    {
                        mark = r;
                        mark.left = mark.right - 20;
                        DrawTextW(d->hDC, L"\x203a", 1, &mark,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                }
            }
            SelectObject(d->hDC, old);
            return TRUE;
        }
    }
    return CallWindowProcW(jm_original_wndproc, hwnd, msg, wp, lp);
}

static void jm_open_mixer(void)
{
    const char *path = getenv("JSRF_MIXER_APP");
    WCHAR exe[32768], command[32772];
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!path || !*path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, exe, 32768))
    {
        MessageBoxW(
            g_menu_hwnd,
            L"The audio mixer companion is not configured. Launch this build using JSRF-Mixer.exe.",
            L"Audio mixer", MB_OK | MB_ICONINFORMATION);
        return;
    }
    _snwprintf(command, 32772, L"\"%ls\"", exe);
    if (CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
        MessageBoxW(g_menu_hwnd, L"Unable to open the audio mixer companion.", L"Audio mixer",
                    MB_OK | MB_ICONERROR);
}
