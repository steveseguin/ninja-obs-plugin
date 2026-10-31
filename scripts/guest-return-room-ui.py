"""Operate and capture only the portable OBS process identified by this review run."""
import ctypes
import json
from pathlib import Path
import sys
import time

from PIL import Image
from pywinauto import Application, Desktop

output = Path(sys.argv[1]).resolve()
state = json.loads((output / 'state.json').read_text())
app = Application(backend='uia').connect(process=state['pid'])
action = sys.argv[2]
windows = app.windows()
main = next(w for w in windows if w.window_text().startswith('OBS '))

def find(identifier):
    for window in app.windows():
        for control in window.descendants():
            if control.element_info.automation_id.split('.')[-1] == identifier:
                return control
    raise RuntimeError('Missing control: ' + identifier)

def native_click(control):
    import win32gui
    window = control
    while window.element_info.control_type != 'Window' or not window.handle:
        window = window.parent()
    point = control.rectangle().mid_point()
    x, y = win32gui.ScreenToClient(window.handle, (point.x, point.y))
    win32gui.SendMessage(window.handle, 0x0201, 1, (y << 16) | x)
    win32gui.SendMessage(window.handle, 0x0202, 0, (y << 16) | x)

if action == 'dump':
    for window in windows:
        print('WINDOW', window.window_text(), window.rectangle())
        for control in window.descendants():
            info = control.element_info
            if control.is_visible():
                print(info.control_type, repr(info.name), repr(info.automation_id), str(control.rectangle()))
elif action == 'open':
    main.set_focus()
    main.maximize()
    main.menu_select('Tools->VDO.Ninja Studio')
elif action == 'tab':
    for window in app.windows():
        matches = [c for c in window.descendants(control_type='TabItem') if c.window_text() == 'Guest Return']
        if matches:
            matches[0].select()
            break
    else: raise RuntimeError('No Guest Return tab')
elif action == 'set':
    values = json.loads(sys.argv[3])
    for identifier, value in values.items():
        control = find(identifier)
        if control.element_info.control_type == 'CheckBox':
            if bool(control.get_toggle_state()) != bool(value): control.toggle()
        elif control.element_info.control_type == 'ComboBox':
            if isinstance(value, int):
                control.set_focus()
                control.type_keys('{ESC}{HOME}' + '{DOWN}' * value + '{ENTER}')
                continue
            try:
                control.select(value)
            except IndexError:
                # Qt can expose its popup outside the combo's UIA subtree.
                # Click the visible item in the owned process's popup instead.
                matches = [c for w in app.windows() for c in w.descendants(control_type='ListItem')
                           if c.window_text() == value and c.is_visible()]
                assert matches, 'Expected a visible combo item'
                matches[0].click_input()
        else:
            control.iface_value.SetValue(str(value))
            control.type_keys('{TAB}')
elif action == 'click':
    control = find(sys.argv[3])
    assert control.is_enabled(), 'Control is disabled'
    if control.element_info.control_type == 'CheckBox': control.toggle()
    else: control.invoke()
elif action == 'combo':
    import win32gui
    dock = find('VDONinjaStudioDock')
    win32gui.MoveWindow(dock.handle, 100, 40, 600, 1100, True)
    control = find(sys.argv[3])
    assert control.is_enabled()
    control.click_input()
    time.sleep(.3)
    choices = [c for w in app.windows() for c in w.descendants(control_type='ListItem')
               if c.window_text() == sys.argv[4] and c.is_visible()]
    assert choices, 'Combo option not visible'
    choices[0].click_input()
    time.sleep(.2)
    assert find(sys.argv[3]).selected_text() == sys.argv[4], 'Combo selection did not apply'
elif action == 'resize':
    import win32gui
    identifier = sys.argv[5] if len(sys.argv) > 5 else 'VDONinjaStudioDock'
    control = find(identifier)
    win32gui.MoveWindow(control.handle, 100 if identifier != 'VDONinjaStudioDock' else 1080,
                       40, int(sys.argv[3]), int(sys.argv[4]), True)
elif action == 'audio-track':
    import win32gui
    dialog = find('OBSAdvAudio')
    controls = dialog.descendants()
    label = next(c for c in controls if c.window_text() == sys.argv[3] and c.element_info.control_type == 'Text')
    row = label.rectangle().mid_point().y
    track = next(c for c in controls if c.window_text() == 'Track ' + sys.argv[4]
                 and abs(c.rectangle().mid_point().y-row) < 15)
    point = track.rectangle().mid_point()
    x, y = win32gui.ScreenToClient(dialog.handle, (point.x, point.y))
    # OBS routes audio from clicked(), not merely a changed checkbox value.
    # Deliver an actual mouse press/release to the owned dialog's checkbox.
    win32gui.SendMessage(dialog.handle, 0x0201, 1, (y << 16) | x)
    win32gui.SendMessage(dialog.handle, 0x0202, 0, (y << 16) | x)
elif action == 'click-name':
    import win32gui
    control = next(c for w in app.windows() for c in w.descendants()
                   if c.window_text() == sys.argv[3] and c.element_info.control_type == sys.argv[4])
    window = control
    while window.element_info.control_type != 'Window' or not window.handle:
        window = window.parent()
    point = control.rectangle().mid_point()
    x, y = win32gui.ScreenToClient(window.handle, (point.x, point.y))
    win32gui.SendMessage(window.handle, 0x0201, 1, (y << 16) | x)
    win32gui.SendMessage(window.handle, 0x0202, 0, (y << 16) | x)
elif action == 'capture':
    name = sys.argv[3]
    # PrintWindow captures the owned application even when the agent's terminal
    # is foreground. Never include unrelated desktop applications in guide assets.
    import win32gui
    import win32ui
    control = find(sys.argv[4]) if len(sys.argv) > 4 else main
    window = control
    while window.element_info.control_type != 'Window' or not window.handle:
        window = window.parent()
    left, top, right, bottom = win32gui.GetWindowRect(window.handle)
    width, height = right-left, bottom-top
    dc = win32gui.GetWindowDC(window.handle)
    source = win32ui.CreateDCFromHandle(dc)
    memory = source.CreateCompatibleDC()
    bitmap = win32ui.CreateBitmap()
    bitmap.CreateCompatibleBitmap(source, width, height)
    memory.SelectObject(bitmap)
    assert ctypes.windll.user32.PrintWindow(window.handle, memory.GetSafeHdc(), 2)
    image = Image.frombuffer('RGB', (width, height), bitmap.GetBitmapBits(True), 'raw', 'BGRX', 0, 1)
    win32gui.DeleteObject(bitmap.GetHandle())
    memory.DeleteDC()
    source.DeleteDC()
    win32gui.ReleaseDC(window.handle, dc)
    folder = output / 'screenshots'
    folder.mkdir(exist_ok=True)
    image.save(folder / (name + '-full.png'))
    rect = control.rectangle()
    box = (max(0, rect.left-left), max(0, rect.top-top), min(width, rect.right-left), min(height, rect.bottom-top))
    image.crop(box).save(folder / (name + '.png'))
    (folder / (name + '-crop.json')).write_text(json.dumps({'source': name + '-full.png', 'box': box}))
    print(str(folder / (name + '.png')))
elif action == 'clipboard':
    import win32clipboard
    win32clipboard.OpenClipboard()
    try: text = win32clipboard.GetClipboardData()
    finally: win32clipboard.CloseClipboard()
    (output / sys.argv[3]).write_text(text, encoding='utf-8')
    print(text)
elif action == 'status':
    for key in ['GuestReturnStatus', 'GuestReturnStart', 'GuestReturnStop', 'GuestReturnCopyInvite',
                'GuestReturnMaxViewers', 'GuestReturnSignaling']:
        try: c = find(key)
        except RuntimeError: continue  # Collapsed advanced controls are absent from UIA.
        print(key, repr(c.window_text()), c.is_enabled())
elif action == 'close':
    ctypes.windll.user32.PostMessageW(main.handle, 0x0010, 0, 0)
else:
    raise RuntimeError('Unknown action')
