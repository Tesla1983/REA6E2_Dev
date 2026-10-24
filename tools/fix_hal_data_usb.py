"""给 ra_gen/hal_data.h 补上裸机版 USB 回调声明（可重复执行, 幂等）。

背景
----
RASC 生成 hal_data.h 时, USB 回调只生成了 RTOS 版:

    #if 2 == BSP_CFG_RTOS
    void usb_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
    #endif

而本工程是裸机(BSP_CFG_RTOS == 0), 回调签名是 `void (*)(usb_callback_args_t *)`。
结果是 hal_data.c 里 `.p_usb_apl_callback = usb_callback,` 报 "undeclared"。

hal_data.h 是 RASC 生成物, 每次 `rascc --generate` 都会覆盖, 所以不放手工补丁,
而是用本脚本在生成之后补一行。生成流程变成:

    rascc --generate ... configuration.xml
    python tools/fix_hal_data_usb.py projects/ra6e2_66_app
"""

import io
import os
import sys

MARKER = '/* bare-metal usb_callback (added by tools/fix_hal_data_usb.py) */'

BLOCK = '''#if 0 == BSP_CFG_RTOS
%s
void usb_callback(usb_callback_args_t * p_args);
#endif
''' % MARKER


def main(project_dir):
    path = os.path.join(project_dir, 'ra_gen', 'hal_data.h')
    s = io.open(path, encoding='utf-8').read()

    if MARKER in s:
        print('already patched: %s' % path)
        return 0

    anchor = '''#if 2 == BSP_CFG_RTOS
#ifndef usb_callback
void usb_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
#endif
#endif
'''
    if anchor not in s:
        print('[error] 找不到 USB 回调声明块, hal_data.h 结构可能变了')
        return 1

    s = s.replace(anchor, anchor + BLOCK, 1)
    io.open(path, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched %s' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
