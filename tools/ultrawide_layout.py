# Adapted from MacNativeGames games/totk/scripts/ultrawide_layout.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors). Used by tools/make_romfs_patches.py --aspect; the runtime has no UI squeeze of its own, so
# the note below about nvn_mtl.m does not apply here.
"""The game's UI layouts for a canvas wider than 16:9 (TOTK_OUTPUT_RES=3440x1440 with TOTK_ASPECT), edited in place in
the decompressed UI/LayoutArchive/Common.Product.*.blarc (scripts/make_romfs_patches.py --aspect).

The game lays its UI out on a 1920x1080 grid and maps that grid onto the canvas, so on a 21:9 canvas every layout comes
out 34% too wide. Here each layout's root pane is narrowed to 16:9 (scale x = 16/9 over the aspect), backgrounds that
must cover the whole screen are widened back, and the HUD's parts move out to the screen's corners. The pane list and
the distances are the ones the community TOTK-Ultrawide mod for emulators found for 1.0.0 .. 1.2.1; this is an
independent implementation (none of the mod's code), checked against 1.4.3's Common.Product.140: every pane is found by
name and every pane field and animation key holds the value the edit expects before it is written.

The runtime's own UI squeeze (nvn_mtl.m, TOTK_UI_SQUEEZE) must be off with these layouts, or the UI is narrowed twice.
"""
import struct

# BFLYT pane sections (pan1, pic1, txt1, ...): name at +12 (24 bytes), translate x / y at +44 / +48, scale x / y at +68 / +72.
PANE_MAGICS = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
FIELD = {'x': 44, 'y': 48, 'scale_x': 68, 'scale_y': 72}

# Full-screen layouts whose root pane stays as wide as the canvas.
KEEP_ROOT = {'AppMenuBG_00', 'PauseMenuBG_00', 'ShopMenuParasailBG_00', 'ShortCutBG_00', 'TitleMenuBG_00', 'EventFade_00',
             'EventFadeWipe_00', 'CameraShutter_00', 'PaAppPictureBookDetail_00', 'AppMenuOverlay_00'}

def edits(k, hud_corner):
    """(layout, pane, field, value) for the aspect: k = 16/9 over the aspect (the root's scale), w = how far a 1920-wide
    layout's edge moves out in its own units (960 * (1 / k - 1)). OFF moves a pane out of sight."""
    w, OFF = 960 * (1 / k - 1), 10000.0
    wide = 1 / k
    out = [
        ('AppAlbum_00', 'N_FadeInOut_00', 'scale_x', k), ('AppAlbum_00', 'N_Open_01', 'scale_x', wide),
        *[('AppMap_00', f'N_BlurInOut_0{i}', 'scale_x', wide) for i in range(4)],
        ('AppMap_00', 'N_MiniMap_00', 'x', -270 * (1 - k)),
        ('AppMenuOverlay_00', 'N_FadeInOut_00', 'scale_x', k), ('AppMenuOverlay_00', 'N_FadeInOut_01', 'scale_x', k),
        ('AppMenuOverlay_00', 'N_Icon_00', 'scale_x', k), ('AppMenuOverlay_00', 'N_TitleOne_01', 'scale_x', k),
        ('AppOpenDemoWindow_00', 'P_BG_00', 'scale_x', wide), ('AutoBuilder_00', 'N_BG_00', 'scale_x', wide),
        ('CameraPointer_00', 'N_InOutScope_00', 'scale_x', wide), ('CameraPointer_00', 'N_InOutScope_00', 'scale_y', wide),
        ('CameraPointer_00', 'N_Type_01', 'scale_x', k), ('CameraPointer_00', 'Pa_CameraFocus_00', 'scale_x', wide),
        ('CameraPointer_00', 'S_Scissor_00', 'scale_x', wide), ('CameraPointer_00', 'W_Shadow_01', 'scale_x', wide),
        ('CameraSystemWindow_00', 'N_AlbumCon_00', 'scale_x', k), ('CameraSystemWindow_00', 'N_InOut_00', 'scale_x', wide),
        ('CameraSystemWindow_00', 'Pa_BtnL_00', 'scale_x', k), ('CameraSystemWindow_00', 'Pa_BtnR_00', 'scale_x', k),
        ('CameraSystemWindow_00', 'T_Text_00', 'scale_x', k),
        ('ChallengeLog_00', 'C_CaptureUp_00', 'x', OFF), ('ChallengeLog_00', 'C_CaptureUp_01', 'x', OFF),
        ('GameOver_00', 'P_BG_00', 'scale_x', wide),
        ('LoadingFade_00', 'N_Base_00', 'scale_x', wide), ('LoadingFade_00', 'N_Logo_00', 'x', w),
        ('LoadingFade_00', 'N_Status_00', 'x', w), ('LoadingFade_00', 'N_Tips_00', 'x', -w),
        ('LoadingFade_00', 'P_BG_00', 'scale_x', wide), ('LoadingFade_00', 'P_SideBGDeco_00', 'scale_x', k),
        ('LoadingFade_00', 'Pa_IconPlayer_00', 'scale_x', k), ('LoadingFade_00', 'Pa_LargeDungeonMap_00', 'scale_x', k),
        ('LoadingFade_00', 'Pa_LargeDungeonMap_01', 'scale_x', k), ('LoadingFade_00', 'Pa_Map_00', 'scale_x', k),
        ('LoadingFade_00', 'Pa_Map_01', 'scale_x', k),
        ('MessageLog_01', 'P_Capture_00', 'scale_x', wide), ('OwnedHorseList_00', 'N_BG_00', 'scale_x', wide),
        ('PaCommonBtnTitle_00', 'B_Hit_00', 'x', OFF), ('PaCommonBtnTitle_00', 'N_Cursor_00', 'x', OFF),
        ('PaLargeDungeonMap_00', 'P_BG_00', 'scale_x', wide), ('PaMessageBtn_00', 'B_Hit_00', 'x', OFF),
        ('PaRecipeCardList_00', 'P_BG_00', 'scale_x', wide),
        ('SaveLoadIcon_00', 'RootPane', 'x', -960 * (1 - k)),
        ('ShopMenu_00', 'N_BG_00', 'scale_x', wide), ('StaffRoll_00', 'N_BG_00', 'scale_x', wide),
    ]
    # Menus framed by two side decorations, a header and a background: the frame stays at the screen's edges.
    for layout, deco in (('PauseMenuOverlay_00', 'P_Deco{}_00'), ('SystemLoadList_00', 'P_Deco{}_00'),
                         ('SystemOption_00', 'P_Deco{}_00'), ('SystemSaveLoad_00', 'P_Deco{}_00')):
        out += [(layout, deco.format('L'), 'scale_x', k), (layout, deco.format('L'), 'x', -(675 + w) * k),
                (layout, deco.format('R'), 'scale_x', k), (layout, deco.format('R'), 'x', (737 + w) * k)]
        if layout == 'PauseMenuOverlay_00':
            out += [(layout, 'P_FooterBase_00', 'scale_x', wide), (layout, 'S_DecoScissor_00', 'scale_x', wide)]
        else:
            out += [(layout, 'A_Title_00', 'scale_x', k), (layout, 'N_Header_00', 'scale_x', wide),
                    (layout, 'N_SystemBG_00', 'scale_x', wide)]
    out += [('SystemActionGuide_00', 'A_Title_00', 'scale_x', k), ('SystemActionGuide_00', 'N_Header_00', 'scale_x', wide),
            ('SystemActionGuide_00', 'N_SystemBG_00', 'scale_x', wide),
            ('SystemActionGuide_00', 'P_DecoL_01', 'scale_x', k), ('SystemActionGuide_00', 'P_DecoL_01', 'x', -(675 + w) * k),
            ('SystemActionGuide_00', 'P_DecoL_02', 'scale_x', k), ('SystemActionGuide_00', 'P_DecoL_02', 'x', (675 + w) * k)]
    if hud_corner:   # the HUD in the screen's corners instead of the middle 16:9
        right = 960 * (1 - k)
        out += [(name, 'RootPane', 'x', right) for name in ('EnvironmentInfo_00', 'EventSkip_00', 'InstantTipsAppMenu_00',
                                                             'InstantTipsWithPict_00', 'InstantTips_00', 'NumDisplaySecond_00',
                                                             'NumDisplay_00', 'PickUpWin_00')]
        out += [('Location_00', 'N_Small_00', 'x', -(388 + w)),
                ('PlayerStatus3D_00', 'N_ExtraStaminaTarget_01', 'x', -(745 + w)),
                ('PlayerStatus_00', 'N_DevilDemoTargetC_00', 'x', -(284 + w)),
                ('PlayerStatus_00', 'N_DevilDemoTargetLT_00', 'x', -(576 + w)),
                ('PlayerStatus_00', 'N_HeartGaugeTargetLT_00', 'x', -(576 + w)),
                ('PlayerStatus_00', 'N_HeartPos_00', 'x', -w),
                ('PlayerStatus_00', 'Pa_CompanionLife_00', 'x', -(840 + w))]
    return out

def anim_edits(k, hud_corner):
    """(animation, byte offset, value the game ships, new value): floats inside BFLAN key tables."""
    w = 960 * (1 / k - 1)
    out = [('PaMapIconDragonTears_00_Zoom', 288, 1.08, 1.08 / k), ('PaMapIconDragonTears_00_Zoom', 300, 1.28, 1.28 / k)]
    if hud_corner:   # the minimap's slide-in keys end at x 765, the right edge of a 16:9 layout
        out += [('AppMap_00_MiniMap', o, 765.0, 765 + w) for o in (872, 880, 1628, 1636, 3264, 3272, 3468, 3476)]
    return out

def panes(blarc, lo, hi):
    """pane name -> absolute section offset, first occurrence, for the BFLYT at blarc[lo:hi]."""
    assert blarc[lo:lo + 4] == b'FLYT', 'not a BFLYT'
    head, count = struct.unpack_from('<H', blarc, lo + 6)[0], struct.unpack_from('<H', blarc, lo + 0x10)[0]
    pos, out = lo + head, {}
    for _ in range(count):
        magic, size = bytes(blarc[pos:pos + 4]), struct.unpack_from('<I', blarc, pos + 4)[0]
        if magic in PANE_MAGICS:
            out.setdefault(bytes(blarc[pos + 12:pos + 36]).split(b'\0')[0].decode(), pos)
        pos += size
    assert pos <= hi
    return out

def find_pane(found, layout, name):
    """The pane of that exact name (a version that renamed it stops the script)."""
    if name not in found: raise SystemExit(f'{layout}: no pane {name}')
    return found[name]

def apply(blarc, members, width, height, hud_corner=True):
    """Edits blarc (bytearray) in place for a width x height canvas. Returns the members it changed."""
    k = (16 / 9) / (width / height)
    assert k < 1, 'only for canvases wider than 16:9'
    changed, layouts = set(), {}
    unknown = sorted(n for n in KEEP_ROOT if f'blyt/{n}.bflyt' not in members)
    if unknown: raise SystemExit(f'KEEP_ROOT names no layout: {unknown}')
    def pane_offset(layout, pane):
        member = f'blyt/{layout}.bflyt'
        if member not in layouts: layouts[member] = panes(blarc, *members[member])
        changed.add(member)
        return find_pane(layouts[member], layout, pane)
    for member in sorted(m for m in members if m.startswith('blyt/') and m.endswith('.bflyt')):
        if member[5:-6] in KEEP_ROOT: continue
        struct.pack_into('<f', blarc, pane_offset(member[5:-6], 'RootPane') + FIELD['scale_x'], k)
    # Every edit's value at k = 1 is the value the game ships (a scale of 1, the 16:9 position): checked before writing,
    # so a version with other numbers stops here instead of getting wrong positions. OFF moves a pane away: not checked.
    for (layout, pane, field, value), (_, _, _, shipped) in zip(edits(k, hud_corner), edits(1.0, hud_corner)):
        at = pane_offset(layout, pane) + FIELD[field]
        have = struct.unpack_from('<f', blarc, at)[0]
        if value != 10000.0 and abs(have - shipped) > 1e-3: raise SystemExit(f'{layout} {pane} {field}: {have}, expected {shipped}')
        struct.pack_into('<f', blarc, at, value)
    for anim, offset, shipped, value in anim_edits(k, hud_corner):
        member = f'anim/{anim}.bflan'; lo, hi = members[member]
        if offset + 4 > hi - lo: raise SystemExit(f'{anim}: too short for +{offset}')
        have = struct.unpack_from('<f', blarc, lo + offset)[0]
        if abs(have - shipped) >= 1e-4: raise SystemExit(f'{anim} +{offset}: {have}, expected {shipped}')
        struct.pack_into('<f', blarc, lo + offset, value); changed.add(member)
    return sorted(changed)
