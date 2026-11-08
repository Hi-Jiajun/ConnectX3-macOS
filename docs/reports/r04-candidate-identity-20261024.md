# R04：候选产物版本身份的实测差异

## 当前增量：.119 构建身份已校验（运行身份仍开放）

2026-10-24：`.119-local` 完整构建通过。`stage-build-identity.py` 从source Info.plist生成私有staged plist和编译头，kmod消费同一numeric release宏，候选标签独立存为`CX3BuildTag`；无显式标签的构建标为`unversioned-local`。7个身份测试（包括真实编译生成头的consumer）及全量40脚本通过。

对保存Mach-O重新解析符号/segment，`_kmod_info`地址`0x7bd78`、version字段文件偏移`507336`，实际版本为`0.44.48`，与保存bundle的`CFBundleVersion`及source模板相同；staged `CX3BuildTag=0.44.119-local`。72条源/测试输入构建前后与tar一致，包含未改写的source模板。原始机器记录见[verification.json](../../../outputs/20261024-cx3-pcie-query/verification.json)和[完整验收](../../../outputs/20261024-cx3-pcie-query/README.md)。

该增量只解决构建元数据生成/保存产物一致性，未实现boot/run绑定，也未检查当前加载版本；R04整体仍PARTIAL。下文为`.118`发现时的原始事实，不能当作`.119`当前状态。

2026-10-24。仅检查保存的`.118`产物和对应源码，无设备访问，不表示当前已加载版本。此发现扩大R04的明确待办，不能把构建标签当作完整运行/boot身份。

## 已确认事实

对[.118保存产物](../../../outputs/20261024-cx3-adapter-export/README.md)离线检查得到三种不同标记：

| 位置 | 值 | 证据 |
| --- | --- | --- |
| 编译诊断 `CX3_BUILD_TAG` | `0.44.118-local` | build-env、构建命令、二进制strings精确匹配；`build.sh:44,61`注入，`src/Cx3Mlx4.cpp:7128`打印 |
| 包内`CFBundleVersion` / `CFBundleShortVersionString` | 均为`0.44.48` | 对保存Info.plist执行PlistBuddy；build.sh复制模板，没有按BUILD_TAG改写 |
| Mach-O `_kmod_info.version` | `0.20.0` | `src/kmod_info.cpp`的KMOD_EXPLICIT_DECL硬编码；对保存Mach-O定位符号并读取字段核对 |

原始输出：[identity-observation.log](../../../outputs/20261024-cx3-adapter-export/identity-observation.log)。不是仅凭源码推测：`nm -nm`定位`_kmod_info`于`(__DATA,__data)`地址`0x79d58`；`otool -l`给出__DATA vmaddr与fileoff均为`0x78000`，故此产物对应文件偏移同值。按SDK中`kmod_info_64_v1`布局，version位于结构起始后80字节；读取64字节得到ASCII `0.20.0`及NUL填充。该偏移只适用于此已绑定哈希的Mach-O，不能复制到其他构建。SDK定义见`/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Headers/mach/kmod.h`。

## 判定与未完成项

这不推翻`.118`的离线构建/测试PASS，也不是已证实的加载失败、历史冻屏原因或当前设备状态。包版本、kmod版本和诊断标签可以有不同语义，但目前没有让采集工具可靠区分它们的统一合同；仅看系统显示的版本会把不同候选混同。

R04下一实现必须明确这些字段的语义和生成/记录来源，校验保存产物之间的一致性策略；将精确二进制hash、ABI、构建标签及实际boot/运行身份绑定到采集记录。若保留发布版本与候选标签分离，也必须显式命名和分别验证，不能默默当作同一版本。继续保持provider-local generation与boot/run身份分离；普通strings命中不是运行时已加载证明。

尚未改变模板、构建脚本、kmod源码或任何保存产物；没有为同步标签而盲目替换版本字符串。运行/boot绑定、实际加载身份和R04整体仍未完成。
