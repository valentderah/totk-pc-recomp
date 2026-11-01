# Adapted from MacNativeGames games/totk/scripts/ultrawide_layout.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors). Used by tools/make_romfs_patches.py --aspect.
"""The game's UI layouts for a canvas of another shape than 16:9 (output_res = 3440x1440 or 1280x800 in totk.ini),
edited in place in the decompressed UI/LayoutArchive/Common.Product.*.blarc (tools/make_romfs_patches.py --aspect).

The game lays its UI out on a 1920x1080 grid and maps that grid onto the canvas, so on a 21:9 canvas every layout comes
out 34% too wide and on a 16:10 one 11% too tall. Here each layout's root pane is scaled back to 16:9 on the stretched
axis (scale x = 16/9 over the aspect when wider, scale y = the aspect over 16/9 when narrower), backgrounds that must
cover the whole screen are stretched back, and the HUD's parts move out to the screen's corners (wider) or its top and
bottom edges (narrower). The pane list and the distances are the ones the community TOTK-Ultrawide mod for emulators
found for 1.0.0 .. 1.2.1 (GPL; only those values are taken, no code; THIRD_PARTY_NOTICES.md). This is an independent
implementation, checked against 1.2.1's Common.Product.110 and 1.4.3's Common.Product.140: every pane is found by name
and every pane field and animation key holds the value the edit expects before it is written.
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

SYSTEM_MENUS = ('SystemLoadList_00', 'SystemOption_00', 'SystemSaveLoad_00')

def tall_edits(k):
    """(layout, pane, field, value) for a canvas narrower than 16:9: k = the aspect over 16/9 (the root's scale y),
    h = how far a 1080-high layout's edge moves out in its own units (540 * (1 / k - 1))."""
    h, tall = 540 * (1 / k - 1), 1 / k
    out = [('AppAlbum_00', 'N_FadeInOut_00', 'scale_y', k), ('AppAlbum_00', 'N_Open_01', 'scale_y', tall),
           *[('AppMenuOverlay_00', p, 'scale_y', k) for p in ('N_FadeInOut_00', 'N_FadeInOut_01', 'N_Icon_00',
                                                               'N_TitleOne_01')],
           ('CameraPointer_00', 'N_Type_01', 'scale_y', k), ('CameraSystemWindow_00', 'N_FirstOut_00', 'scale_y', k),
           *[('LoadingFade_00', p, 'scale_y', k) for p in ('Pa_IconPlayer_00', 'Pa_LargeDungeonMap_00',
                                                            'Pa_LargeDungeonMap_01', 'Pa_Map_00', 'Pa_Map_01')],
           *[('AppMap_00', f'N_BlurInOut_0{i}', 'scale_y', tall) for i in range(4)],
           *[(layout, 'P_BG_00', 'scale_y', tall) for layout in ('AppOpenDemoWindow_00', 'GameOver_00', 'LoadingFade_00',
                                                                 'PaLargeDungeonMap_00', 'PaRecipeCardList_00')],
           ('LoadingFade_00', 'N_Base_00', 'scale_y', tall),
           *[(layout, 'N_BG_00', 'scale_y', tall) for layout in ('AutoBuilder_00', 'OwnedHorseList_00', 'ShopMenu_00',
                                                                 'StaffRoll_00')],
           *[('CameraPointer_00', p, 'scale_y', tall) for p in ('N_InOutScope_00', 'Pa_CameraFocus_00', 'S_Scissor_00',
                                                                 'W_Shadow_01')],
           ('CameraSystemWindow_00', 'N_InOut_00', 'scale_y', tall), ('MessageLog_01', 'P_Capture_00', 'scale_y', tall),
           *[(layout, 'N_SystemBG_00', 'scale_y', tall) for layout in ('SystemActionGuide_00',) + SYSTEM_MENUS]]
    # The HUD at the screen's top and bottom edges instead of the middle 16:9
    out += [(name, 'RootPane', 'y', 540 * (1 - k)) for name in (
        'AppAlbum_00', 'AppPictureBook_00', 'ChallengeLog_00', 'Challenge_00', 'InstantTipsWithPict_00', 'InstantTips_00',
        'MiniGameTimer_00', 'NumDisplaySecond_00', 'NumDisplay_00', 'PickUpWin_00', 'PlayerStatus_00', 'Pouch_00',
        'ShopMenu_00', 'SystemActionGuide_00') + SYSTEM_MENUS]
    out += [(name, 'RootPane', 'y', -540 * (1 - k)) for name in (
        'CtrlGuide_00', 'DoCommand_00', 'EnvironmentInfo_00', 'HorseGauge_00', 'MessageDialog_00', 'MessageLog_01',
        'PauseMenuOverlay_00', 'SaveLoadIcon_00', 'TreasureFullShortCut_00')]
    out += [('AppAlbum_00', 'N_PicLoad_01', 'y', -540 * (1 - k)), ('AppMap_00', 'N_AllMapTime_00', 'y', -h),
            ('AppMenuOverlay_00', 'A_Align_00', 'y', -(505 + h)), ('AppMenuOverlay_00', 'Pa_GuideLeft_00', 'y', -(505 + h)),
            ('AppMenuOverlay_00', 'N_TitleTab_00', 'y', 491 + h),
            ('LoadingFade_00', 'N_Logo_00', 'y', -h), ('LoadingFade_00', 'N_Sage_00', 'y', -477 - 2 * h),
            ('LoadingFade_00', 'N_Status_00', 'y', h), ('LoadingFade_00', 'N_Tips_00', 'y', -(360 + h)),
            ('Location_00', 'N_Large_00', 'y', 270 + h), ('Location_00', 'N_Small_00', 'y', -(395 + h)),
            ('MessageLog_01', 'P_Capture_00', 'y', h),
            ('PaExtraStaminaGauge_00', 'N_BasePattern_00', 'y', h), ('PaExtraStaminaGauge_00', 'N_GaugeMainPattern_00', 'y', h),
            ('Pouch_00', 'N_Info_00', 'y', -h), ('Pouch_00', 'Pa_ExtraStamina_00', 'y', -h),
            ('ShopMenu_00', 'A_AlignGuide_00', 'y', -(477 + h)),
            *[(layout, 'N_SystemBG_00', 'y', -h) for layout in ('SystemActionGuide_00',) + SYSTEM_MENUS],
            ('SystemLoadList_00', 'Pa_Guide_00', 'y', -(474 + h)), ('SystemOption_00', 'A_Guide_00', 'y', -(474 + h))]
    return out

# BFLAN animation targets: FLPA (pane SRT) translate y, FLTS (texture SRT) scale y
FLPA_Y, FLTS_SY = 1, 4

def anim_keys(blarc, lo, pane, tag, target):
    """The absolute offsets of each key (frame, value, slope: 12 bytes) of the hermite curves of one pane, animation
    tag (b'FLPA', b'FLTS') and target in the BFLAN at blarc[lo:], one list per curve. Found by walking the pai1 section,
    so they hold in any version whose animation has that curve."""
    if blarc[lo:lo + 4] != b'FLAN': raise SystemExit(f'{lo:#x}: not a BFLAN')
    pos, out = lo + struct.unpack_from('<H', blarc, lo + 6)[0], []
    for _ in range(struct.unpack_from('<H', blarc, lo + 0x10)[0]):
        magic, size = bytes(blarc[pos:pos + 4]), struct.unpack_from('<I', blarc, pos + 4)[0]
        if magic == b'pai1':
            table = pos + struct.unpack_from('<I', blarc, pos + 16)[0]
            for e in range(struct.unpack_from('<H', blarc, pos + 14)[0]):
                eo = pos + struct.unpack_from('<I', blarc, table + 4 * e)[0]
                if bytes(blarc[eo:eo + 28]).split(b'\0')[0].decode() != pane: continue
                for t in range(blarc[eo + 28]):
                    to = eo + struct.unpack_from('<I', blarc, eo + 32 + 4 * t)[0]
                    if bytes(blarc[to:to + 4]) != tag: continue
                    for g in range(blarc[to + 4]):
                        go = to + struct.unpack_from('<I', blarc, to + 8 + 4 * g)[0]
                        if blarc[go + 1] != target: continue
                        if blarc[go + 2] != 2: raise SystemExit(f'{pane}: a {tag.decode()} curve that is not hermite')
                        keys = go + struct.unpack_from('<I', blarc, go + 8)[0]
                        out.append([keys + 12 * i for i in range(struct.unpack_from('<H', blarc, go + 4)[0])])
        pos += size
    return out

def tall_anim(blarc, members, k, changed):
    """The animation keys that hold positions or zooms on the screen's height, for a canvas narrower than 16:9."""
    h = 540 * (1 / k - 1)
    get = lambda at: struct.unpack_from('<f', blarc, at)[0]
    put = lambda at, v: struct.pack_into('<f', blarc, at, v)
    def anim(name):
        member = f'anim/{name}.bflan'
        if member not in members: raise SystemExit(f'{name}: no animation {member} in the archive')
        changed.add(member)
        return members[member][0]
    # The minimap's four cut panes slide in on straight lines from 0 at frame 0 to y -360, the bottom of a 16:9 layout
    # (hermite keys: the first one's slope is the end point); the end point moves with the screen's edge.
    lo = anim('AppMap_00_MiniMap')
    for pane in ('N_Cut_00', 'N_Cut_01', 'N_Cut_02', 'N_Cut_03'):
        curves = anim_keys(blarc, lo, pane, b'FLPA', FLPA_Y)
        if len(curves) != 1 or len(curves[0]) != 2: raise SystemExit(f'AppMap_00_MiniMap {pane}: {len(curves)} y curves')
        k0, k1 = curves[0]
        if not (get(k0) == 0 and get(k0 + 4) == 0 and abs(get(k0 + 8) + 360) < 1e-3 and get(k1) == 1 and
                abs(get(k1 + 4) + 360) < 1e-3 and get(k1 + 8) == 0):
            raise SystemExit(f'AppMap_00_MiniMap {pane}: not a line from 0 to y -360')
        put(k0 + 8, -(360 + h)), put(k1 + 4, -(360 + h))
    # The large place name of a discovery holds its height (285, then 260) in flat keys; they move to the top edge.
    curves = anim_keys(blarc, anim('Location_00_IsDiscovery'), 'N_Large_00', b'FLPA', FLPA_Y)
    values = [get(key + 4) for keys in curves for key in keys]
    if len(curves) != 1 or values != [285, 285, 260]: raise SystemExit(f'Location_00_IsDiscovery N_Large_00: keys {values}')
    for key in curves[0]: put(key + 4, get(key + 4) + h)
    # The dragon's tears map icon zooms its picture to 1.08 and 1.28 in flat keys; on the stretched axis by 1 / k more.
    curves = anim_keys(blarc, anim('PaMapIconDragonTears_00_Zoom'), 'P_Geoglyph_00', b'FLTS', FLTS_SY)
    values = [round(get(key + 4), 4) for keys in curves for key in keys]
    if len(curves) != 1 or values != [1.08, 1.28]: raise SystemExit(f'PaMapIconDragonTears_00_Zoom P_Geoglyph_00: keys {values}')
    for key in curves[0]: put(key + 4, get(key + 4) / k)

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
    """Edits blarc (bytearray) in place for a width x height canvas of another shape than 16:9. Returns the members it
    changed. hud_corner (wider canvases): the HUD goes to the screen's corners; a narrower one always has it at the
    top and bottom edges."""
    wide = width / height > 16 / 9
    k = (16 / 9) / (width / height) if wide else (width / height) / (16 / 9)
    assert k < 1, 'only for canvases of another shape than 16:9'
    root_scale = 'scale_x' if wide else 'scale_y'
    changed, layouts = set(), {}
    unknown = sorted(n for n in KEEP_ROOT if f'blyt/{n}.bflyt' not in members)
    if unknown: raise SystemExit(f'KEEP_ROOT names no layout: {unknown}')
    def pane_offset(layout, pane):
        member = f'blyt/{layout}.bflyt'
        if member not in members: raise SystemExit(f'{layout}: no layout {member} in the archive')
        if member not in layouts: layouts[member] = panes(blarc, *members[member])
        changed.add(member)
        return find_pane(layouts[member], layout, pane)
    for member in sorted(m for m in members if m.startswith('blyt/') and m.endswith('.bflyt')):
        if member[5:-6] in KEEP_ROOT: continue
        at = pane_offset(member[5:-6], 'RootPane') + FIELD[root_scale]
        have = struct.unpack_from('<f', blarc, at)[0]
        if abs(have - 1.0) > 1e-3: raise SystemExit(f'{member[5:-6]} RootPane {root_scale}: {have}, expected 1.0')
        struct.pack_into('<f', blarc, at, k)
    # Every edit's value at k = 1 is the value the game ships (a scale of 1, the 16:9 position): checked before writing,
    # so a version with other numbers stops here instead of getting wrong positions. OFF moves a pane away: not checked.
    table = zip(edits(k, hud_corner), edits(1.0, hud_corner)) if wide else zip(tall_edits(k), tall_edits(1.0))
    for (layout, pane, field, value), (_, _, _, shipped) in table:
        at = pane_offset(layout, pane) + FIELD[field]
        have = struct.unpack_from('<f', blarc, at)[0]
        if value != 10000.0 and abs(have - shipped) > 1e-3: raise SystemExit(f'{layout} {pane} {field}: {have}, expected {shipped}')
        struct.pack_into('<f', blarc, at, value)
    if not wide:
        tall_anim(blarc, members, k, changed)
        return sorted(changed)
    for anim, offset, shipped, value in anim_edits(k, hud_corner):
        member = f'anim/{anim}.bflan'
        if member not in members: raise SystemExit(f'{anim}: no animation {member} in the archive')
        lo, hi = members[member]
        if offset + 4 > hi - lo: raise SystemExit(f'{anim}: too short for +{offset}')
        have = struct.unpack_from('<f', blarc, lo + offset)[0]
        if abs(have - shipped) >= 1e-4: raise SystemExit(f'{anim} +{offset}: {have}, expected {shipped}')
        struct.pack_into('<f', blarc, lo + offset, value); changed.add(member)
    return sorted(changed)
