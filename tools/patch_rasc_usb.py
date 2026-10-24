"""把 USB CDC(PCDC) 模块与 240MHz/48MHz 时钟写进 RASC 的 configuration.xml。

用法: python tools/patch_rasc_usb.py <configuration.xml>

设计说明
--------
RASC 生成物里 r_usb_basic_api.h / r_usb_pcdc_api.h / r_usb_basic_cfg.h 都是
RASC 从模块描述模板渲染出来的, FSP 安装包里并没有这些文件。所以想让工程里有
USB 驱动, 只能让 RASC 自己生成 —— 本脚本就是把模块条目补进 XML, 之后跑
`rascc --generate` 即可。

属性 id 全部取自 RASC 自己导出的模块描述:
  rascc --extractsupportfiles <dir>
  internal/projectgen/ra/modules/Renesas##HAL Drivers##all##r_usb_basic####6.6.0.xml
"""

import io
import sys

CLOCK_REPLACEMENTS = [
    ('<node id="board.clock.pll.mul" option="board.clock.pll.mul.100"/>',
     '<node id="board.clock.pll.mul" option="board.clock.pll.mul.120"/>'),
    ('<node id="board.clock.uclk.source" option="board.clock.uclk.source.disabled"/>',
     '<node id="board.clock.uclk.source" option="board.clock.uclk.source.pll"/>'),
    ('<node id="board.clock.iclk.div" option="board.clock.iclk.div.1"/>',
     '<node id="board.clock.iclk.div" option="board.clock.iclk.div.2"/>'),
    ('<node id="board.clock.pclka.div" option="board.clock.pclka.div.2"/>',
     '<node id="board.clock.pclka.div" option="board.clock.pclka.div.4"/>'),
    ('<node id="board.clock.pclkb.div" option="board.clock.pclkb.div.4"/>',
     '<node id="board.clock.pclkb.div" option="board.clock.pclkb.div.8"/>'),
    ('<node id="board.clock.pclkc.div" option="board.clock.pclkc.div.4"/>',
     '<node id="board.clock.pclkc.div" option="board.clock.pclkc.div.8"/>'),
    ('<node id="board.clock.pclkd.div" option="board.clock.pclkd.div.2"/>',
     '<node id="board.clock.pclkd.div" option="board.clock.pclkd.div.4"/>'),
    ('<node id="board.clock.fclk.div" option="board.clock.fclk.div.4"/>',
     '<node id="board.clock.fclk.div" option="board.clock.fclk.div.8"/>'),
]

# ⚠ 必须用 _nodmac 变体: 普通 basic_on_usb 要求 interface.mcu.usb.pipes.0.1.2.3.4.5.6.7.8.9
#   (9 个管道), 而 RA6E2 只有 pipe 0/4/5/6/7 (BSP_FEATURE_USB_HAS_PIPE04567=1)。
#   用了 basic_on_usb 会让 rascc 报 "Required element 'Requires USB Full Module' is not available"。
USB_BASIC_ID = 'module.driver.basic_on_usb_nodmac.1837466192'
USB_PCDC_ID = 'module.driver.pcdc_on_usb.2077813593'

COMPONENTS = '''    <component apiversion="" class="HAL Drivers" condition="" group="all" subgroup="r_usb_basic" variant="" vendor="Renesas" version="6.6.0">
      <description>USB Basic</description>
      <originalPack>Renesas.RA.6.6.0.pack</originalPack>
    </component>
    <component apiversion="" class="HAL Drivers" condition="" group="all" subgroup="r_usb_pcdc" variant="" vendor="Renesas" version="6.6.0">
      <description>USB PCDC</description>
      <originalPack>Renesas.RA.6.6.0.pack</originalPack>
    </component>
'''

MODULES = '''    <module id="{basic}">
      <property id="module.driver.basic.basic_interface" value="basic"/>
      <property id="module.driver.basic.name" value="g_basic0"/>
      <property id="module.driver.usb_basic.usb_mode" value="module.driver.usb_basic.usb_mode.peri"/>
      <property id="module.driver.usb_basic.usb_speed" value="module.driver.usb_basic.usb_speed.fs"/>
      <property id="module.driver.usb_basic.usb_modulenumber" value="module.driver.usb_basic.usb_modulenumber.0"/>
      <property id="module.driver.usb_basic.usb_classtype" value="module.driver.usb_basic.usb_classtype.pcdc"/>
      <property id="module.driver.usb_basic.p_usb_reg" value="g_usb_descriptor"/>
      <property id="module.driver.usb_basic.complience_cb" value="NULL"/>
      <property id="module.driver.usb_basic.ipl" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.ipl_r" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.ipl_d0" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.ipl_d1" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.hsipl" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.hsipl_d0" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.hsipl_d1" value="board.icu.common.irq.priority12"/>
      <property id="module.driver.usb_basic.rtos_callback" value="usb_callback"/>
      <property id="module.driver.usb_basic.other_context" value="NULL"/>
    </module>
    <module id="{pcdc}">
      <property id="module.driver.pcdc.name" value="g_pcdc0"/>
      <property id="module.driver.pcdc.usb_basic_name" value="g_basic0"/>
    </module>
'''.format(basic=USB_BASIC_ID, pcdc=USB_PCDC_ID)

# ⚠ 关键: PCDC 模块 `requires` basic_on_usb, 在 RASC 的 XML 里必须把 basic 嵌套在
#   pcdc 的 <stack> 内部, 否则 RASC 解析不出依赖, 会静默跳过 PCDC 模块实例
#   => USB_CFG_PCDC_USE 不被定义 => PCDC class driver 编译不进工程 => 枚举失败。
#   之前写成并列的两个 <stack> 就是这个坑的根因。
STACKS = '''
      <stack module="{pcdc}">
        <stack module="{basic}"/>
      </stack>'''.format(basic=USB_BASIC_ID, pcdc=USB_PCDC_ID)

USB_CFG = '''    <config id="config.driver.usb_basic">
      <property id="config.driver.usb_basic.param_checking_enable" value="config.driver.usb_basic.param_checking_enable.bsp"/>
      <property id="config.driver.usb_basic.pll_clock_frequency" value="config.driver.usb_basic.pll_clock_frequency.24mhz"/>
      <property id="config.driver.usb_basic.buswait" value="config.driver.usb_basic.buswait.7"/>
      <property id="config.driver.usb_basic.bc_function" value="config.driver.usb_basic.bc_function.disable"/>
      <property id="config.driver.usb_basic.power_source" value="config.driver.usb_basic.power_source.high"/>
      <property id="config.driver.usb_basic.dcp_function" value="config.driver.usb_basic.dcp_function.disable"/>
      <property id="config.driver.usb_basic.request" value="config.driver.usb_basic.request.enable"/>
      <property id="config.driver.usb_basic.dblb" value="config.driver.usb_basic.dblb.enable"/>
      <property id="config.driver.usb_basic.cntmd" value="config.driver.usb_basic.cntmd.disable"/>
      <property id="config.driver.usb_basic.ldo_regulator" value="config.driver.usb_basic.ldo_regulator.disable"/>
      <property id="config.driver.usb_basic.dma" value="config.driver.usb_basic.dma.disable"/>
      <property id="config.driver.usb_basic.usb_multiport" value="config.driver.usb_basic.usb_multiport.disable"/>
      <property id="config.driver.usb_basic.compliance_mode" value="config.driver.usb_basic.compliance_mode.disable"/>
    </config>
'''


def main(path):
    s = io.open(path, encoding='utf-8').read()
    if USB_BASIC_ID in s:
        print('already patched')
        return 0

    for old, new in CLOCK_REPLACEMENTS:
        if old in s:
            s = s.replace(old, new, 1)
        else:
            print('  [warn] clock node not found: %s' % old[:60])

    s = s.replace('  </raComponentSelection>', COMPONENTS + '  </raComponentSelection>', 1)
    s = s.replace('    <context id="_hal.0">',
                  '    <context id="_hal.0">' + STACKS, 1)
    s = s.replace('  </raModuleConfiguration>', USB_CFG + '  </raModuleConfiguration>', 1)
    s = s.replace('    <context id="_hal.0">', MODULES + '    <context id="_hal.0">', 1)

    io.open(path, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched %s' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
