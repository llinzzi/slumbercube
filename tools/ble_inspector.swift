#!/usr/bin/env swift

import CoreBluetooth
import Foundation

struct Options {
    var scanSeconds: TimeInterval = 10
    var inspectSelector: String?
    var readValues = false
    var writeSelector: String?
    var writeCharacteristic: String?
    var writeValue: Data?
    var confirmedWrite = false
    var showHelp = false
}

struct SeenDevice {
    let peripheral: CBPeripheral
    var name: String
    var rssi: Int
    var connectable: Bool?
    var manufacturerData: Data?
    var serviceUUIDs: [CBUUID]
    var serviceData: [CBUUID: Data]
    var txPower: Int?
    var sightings: Int
}

private func usage() {
    print("""
    BLE Inspector — macOS 低功耗蓝牙扫描与 GATT 能力检查

    用法:
      ble-inspector [--seconds 10]
      ble-inspector --inspect <设备名|UUID|all> [--read] [--seconds 10]
      ble-inspector --write <设备名|UUID> <特征UUID> <十六进制值> --yes

    示例:
      ble-inspector --seconds 15
      ble-inspector --inspect blinkwh
      ble-inspector --inspect 12345678-1234-1234-1234-123456789ABC --read
      ble-inspector --write blinkwh FFE1 01 --yes

    说明:
      --inspect 连接设备并列出服务、特征以及 read/write/notify 能力。
      --read    读取标记为可读的特征；设备可能要求配对或授权。
      --write   只应用于你拥有或获准测试的设备，并且必须显式加 --yes。
      macOS CoreBluetooth 不公开设备真实 MAC 地址，只显示系统分配的 UUID。
    """)
}

private func parseHex(_ text: String) -> Data? {
    let compact = text
        .replacingOccurrences(of: "0x", with: "", options: .caseInsensitive)
        .filter { !$0.isWhitespace && $0 != ":" && $0 != "-" }
    guard !compact.isEmpty, compact.count % 2 == 0,
          compact.allSatisfy({ $0.isHexDigit }) else { return nil }
    var bytes: [UInt8] = []
    var index = compact.startIndex
    while index < compact.endIndex {
        let next = compact.index(index, offsetBy: 2)
        guard let value = UInt8(compact[index..<next], radix: 16) else { return nil }
        bytes.append(value)
        index = next
    }
    return Data(bytes)
}

private func parseOptions() -> Options? {
    var options = Options()
    let args = Array(CommandLine.arguments.dropFirst())
    var i = 0
    while i < args.count {
        switch args[i] {
        case "-h", "--help":
            options.showHelp = true
            i += 1
        case "--seconds":
            guard i + 1 < args.count, let seconds = Double(args[i + 1]),
                  seconds >= 1, seconds <= 120 else {
                fputs("--seconds 必须是 1 到 120 之间的数字\n", stderr)
                return nil
            }
            options.scanSeconds = seconds
            i += 2
        case "--inspect":
            guard i + 1 < args.count else {
                fputs("--inspect 后需要设备名、设备 UUID 或 all\n", stderr)
                return nil
            }
            options.inspectSelector = args[i + 1]
            i += 2
        case "--read":
            options.readValues = true
            i += 1
        case "--write":
            guard i + 3 < args.count, let data = parseHex(args[i + 3]) else {
                fputs("--write 格式: --write <设备名|UUID> <特征UUID> <十六进制值>\n", stderr)
                return nil
            }
            options.writeSelector = args[i + 1]
            options.writeCharacteristic = args[i + 2]
            options.writeValue = data
            i += 4
        case "--yes":
            options.confirmedWrite = true
            i += 1
        default:
            fputs("未知参数: \(args[i])\n", stderr)
            return nil
        }
    }
    if options.writeSelector != nil && !options.confirmedWrite {
        fputs("写入会改变设备状态；确认设备属于你或已获授权后，加 --yes 再运行。\n", stderr)
        return nil
    }
    return options
}

private func hex(_ data: Data, limit: Int = 64) -> String {
    let shown = data.prefix(limit).map { String(format: "%02X", $0) }.joined(separator: " ")
    return data.count > limit ? "\(shown) … (\(data.count) bytes)" : "\(shown) (\(data.count) bytes)"
}

private func companyDescription(_ data: Data?) -> String? {
    guard let data, data.count >= 2 else { return nil }
    let id = UInt16(data[data.startIndex]) | (UInt16(data[data.startIndex + 1]) << 8)
    let known: [UInt16: String] = [
        0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung",
        0x00E0: "Google", 0x038F: "Xiaomi", 0x0131: "Cypress/Infineon"
    ]
    return String(format: "Company ID 0x%04X%@", id, known[id].map { " (\($0))" } ?? "")
}

private func normalizedUUID(_ text: String) -> String {
    text.uppercased()
        .replacingOccurrences(of: "0X", with: "")
        .replacingOccurrences(of: "-", with: "")
}

private func propertyNames(_ properties: CBCharacteristicProperties) -> [String] {
    var names: [String] = []
    if properties.contains(.read) { names.append("read/读取") }
    if properties.contains(.write) { names.append("write/写入(有响应)") }
    if properties.contains(.writeWithoutResponse) { names.append("write-no-response/写入(无响应)") }
    if properties.contains(.notify) { names.append("notify/通知") }
    if properties.contains(.indicate) { names.append("indicate/指示") }
    if properties.contains(.broadcast) { names.append("broadcast/广播") }
    if properties.contains(.authenticatedSignedWrites) { names.append("signed-write/签名写") }
    if properties.contains(.extendedProperties) { names.append("extended/扩展属性") }
    if properties.contains(.notifyEncryptionRequired) { names.append("notify-encrypted/加密通知") }
    if properties.contains(.indicateEncryptionRequired) { names.append("indicate-encrypted/加密指示") }
    return names
}

private func knownUUIDName(_ uuid: CBUUID) -> String? {
    let names = [
        "1800": "Generic Access", "1801": "Generic Attribute",
        "180A": "Device Information", "180F": "Battery Service",
        "181A": "Environmental Sensing", "2A00": "Device Name",
        "2A19": "Battery Level", "2A24": "Model Number",
        "2A25": "Serial Number", "2A26": "Firmware Revision",
        "2A27": "Hardware Revision", "2A29": "Manufacturer Name",
        "FFE0": "blinkwh 灯服务", "FFE1": "blinkwh LED 状态"
    ]
    return names[uuid.uuidString.uppercased()]
}

final class BLEInspector: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let options: Options
    private var central: CBCentralManager!
    private var devices: [UUID: SeenDevice] = [:]
    private var scanTimer: Timer?
    private var pendingInspection: [CBPeripheral] = []
    private var activePeripheral: CBPeripheral?
    private var pendingServices = 0
    private var pendingReads = 0
    private var writeStarted = false
    private var writeFinished = false
    private var exitCode: Int32 = 0

    init(options: Options) {
        self.options = options
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("正在扫描 \(Int(options.scanSeconds)) 秒…")
            central.scanForPeripherals(
                withServices: nil,
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
            )
            scanTimer = Timer.scheduledTimer(
                withTimeInterval: options.scanSeconds,
                repeats: false
            ) { [weak self] _ in self?.finishScan() }
        case .unauthorized:
            fail("没有蓝牙权限。请在“系统设置 → 隐私与安全性 → 蓝牙”中允许 Terminal/Codex。")
        case .poweredOff:
            fail("蓝牙当前已关闭。")
        case .unsupported:
            fail("这台 Mac 不支持 Bluetooth Low Energy。")
        case .resetting:
            print("蓝牙正在重置，等待恢复…")
        case .unknown:
            break
        @unknown default:
            fail("未知的蓝牙状态。")
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let name = localName ?? peripheral.name ?? "(未命名)"
        let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] ?? [:]
        let manufacturer = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data
        let connectable = (advertisementData[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue
        let txPower = (advertisementData[CBAdvertisementDataTxPowerLevelKey] as? NSNumber)?.intValue
        let oldCount = devices[peripheral.identifier]?.sightings ?? 0
        devices[peripheral.identifier] = SeenDevice(
            peripheral: peripheral, name: name, rssi: RSSI.intValue,
            connectable: connectable, manufacturerData: manufacturer,
            serviceUUIDs: services, serviceData: serviceData,
            txPower: txPower, sightings: oldCount + 1
        )
    }

    private func finishScan() {
        central.stopScan()
        let sorted = devices.values.sorted {
            if $0.rssi == $1.rssi { return $0.name < $1.name }
            return $0.rssi > $1.rssi
        }
        print("\n发现 \(sorted.count) 台 BLE 设备（按信号强度排序）")
        if sorted.isEmpty {
            fail("没有收到 BLE 广播。请确认蓝牙权限，并让目标设备处于开启/配对状态。")
            return
        }
        for (index, device) in sorted.enumerated() {
            let connectText = device.connectable.map { $0 ? "可连接" : "仅广播" } ?? "连接性未知"
            print("\n[\(index + 1)] \(device.name)  RSSI \(device.rssi) dBm  \(connectText)")
            print("    UUID: \(device.peripheral.identifier.uuidString)")
            if let tx = device.txPower { print("    Tx Power: \(tx) dBm") }
            if !device.serviceUUIDs.isEmpty {
                print("    广播服务: \(device.serviceUUIDs.map(\.uuidString).joined(separator: ", "))")
            }
            if let data = device.manufacturerData {
                let company = companyDescription(data).map { "  \($0)" } ?? ""
                print("    厂商数据: \(hex(data))\(company)")
            }
            for (uuid, data) in device.serviceData.sorted(by: { $0.key.uuidString < $1.key.uuidString }) {
                print("    服务数据 \(uuid.uuidString): \(hex(data))")
            }
            print("    广播次数: \(device.sightings)")
        }

        let selector = options.writeSelector ?? options.inspectSelector
        guard let selector else {
            print("\n提示：用 --inspect <设备名或UUID> 查看具体控制接口。")
            exit(0)
        }
        pendingInspection = selectDevices(selector, from: sorted)
        if options.writeSelector != nil && pendingInspection.count != 1 {
            fail("写入目标必须唯一匹配；当前匹配到 \(pendingInspection.count) 台。请使用上面显示的设备 UUID。")
            return
        }
        if pendingInspection.isEmpty {
            fail("没有找到匹配 “\(selector)” 的设备。")
            return
        }
        inspectNext()
    }

    private func selectDevices(_ selector: String, from devices: [SeenDevice]) -> [CBPeripheral] {
        if selector.lowercased() == "all" {
            return devices.filter { $0.connectable != false }.map(\.peripheral)
        }
        let needle = selector.lowercased()
        return devices.filter {
            $0.name.lowercased().contains(needle) ||
            $0.peripheral.identifier.uuidString.lowercased() == needle
        }.map(\.peripheral)
    }

    private func inspectNext() {
        guard !pendingInspection.isEmpty else { exit(exitCode) }
        let peripheral = pendingInspection.removeFirst()
        activePeripheral = peripheral
        peripheral.delegate = self
        print("\n── 连接 \(peripheral.name ?? "(未命名)") [\(peripheral.identifier.uuidString)] ──")
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("已连接，正在发现 GATT 服务…")
        peripheral.discoverServices(nil)
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        print("连接失败: \(error?.localizedDescription ?? "未知错误")")
        exitCode = 1
        activePeripheral = nil
        inspectNext()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            finishPeripheral(peripheral, message: "服务发现失败: \(error.localizedDescription)")
            return
        }
        let services = peripheral.services ?? []
        if services.isEmpty {
            finishPeripheral(peripheral, message: "设备没有公开 GATT 服务。")
            return
        }
        pendingServices = services.count
        for service in services {
            let known = knownUUIDName(service.uuid).map { " (\($0))" } ?? ""
            print("服务 \(service.uuid.uuidString)\(known)")
            peripheral.discoverCharacteristics(nil, for: service)
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            print("  特征发现失败: \(error.localizedDescription)")
        } else {
            for characteristic in service.characteristics ?? [] {
                let known = knownUUIDName(characteristic.uuid).map { " (\($0))" } ?? ""
                let properties = propertyNames(characteristic.properties)
                print("  特征 \(characteristic.uuid.uuidString)\(known)")
                print("    能力: \(properties.isEmpty ? "无公开操作" : properties.joined(separator: ", "))")
                if characteristic.uuid.uuidString.uppercased() == "FFE1" {
                    print("    已知协议: 写 01=开灯，00=关灯（来自本项目 blinkwh 固件约定）")
                }
                if options.readValues && characteristic.properties.contains(.read) {
                    pendingReads += 1
                    peripheral.readValue(for: characteristic)
                }
                if let wanted = options.writeCharacteristic,
                   normalizedUUID(characteristic.uuid.uuidString) == normalizedUUID(wanted) {
                    performWrite(peripheral, characteristic)
                }
                peripheral.discoverDescriptors(for: characteristic)
            }
        }
        pendingServices -= 1
        finishDiscoveryIfReady(peripheral)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverDescriptorsFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard error == nil else { return }
        for descriptor in characteristic.descriptors ?? [] {
            print("    描述符: \(descriptor.uuid.uuidString)")
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            print("    读取 \(characteristic.uuid.uuidString) 失败: \(error.localizedDescription)")
        } else if let value = characteristic.value {
            let utf8 = String(data: value, encoding: .utf8)
                .flatMap {
                    $0.unicodeScalars.allSatisfy {
                        !CharacterSet.controlCharacters.contains($0)
                    } ? $0 : nil
                }
            print("    读取 \(characteristic.uuid.uuidString): \(hex(value))\(utf8.map { "  UTF-8=\"\($0)\"" } ?? "")")
        }
        pendingReads -= 1
        finishDiscoveryIfReady(peripheral)
    }

    private func performWrite(_ peripheral: CBPeripheral, _ characteristic: CBCharacteristic) {
        guard let data = options.writeValue else { return }
        let type: CBCharacteristicWriteType
        if characteristic.properties.contains(.write) {
            type = .withResponse
        } else if characteristic.properties.contains(.writeWithoutResponse) {
            type = .withoutResponse
        } else {
            print("    拒绝写入：这个特征没有声明写权限。")
            writeFinished = true
            exitCode = 1
            return
        }
        writeStarted = true
        print("    写入 \(characteristic.uuid.uuidString): \(hex(data))")
        peripheral.writeValue(data, for: characteristic, type: type)
        if type == .withoutResponse {
            print("    写入已发送（设备不返回确认）。")
            writeFinished = true
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            print("    写入失败: \(error.localizedDescription)")
            exitCode = 1
        } else {
            print("    写入成功，设备已确认。")
        }
        writeFinished = true
        finishDiscoveryIfReady(peripheral)
    }

    private func finishDiscoveryIfReady(_ peripheral: CBPeripheral) {
        guard pendingServices == 0, pendingReads == 0 else { return }
        if options.writeSelector != nil {
            if writeFinished {
                finishPeripheral(peripheral)
            } else if writeStarted {
                return
            } else {
                finishPeripheral(peripheral, message: "未找到指定特征 \(options.writeCharacteristic ?? "")。")
                exitCode = 1
            }
        } else {
            finishPeripheral(peripheral)
        }
    }

    private func finishPeripheral(_ peripheral: CBPeripheral, message: String? = nil) {
        if let message {
            print(message)
            exitCode = 1
        }
        central.cancelPeripheralConnection(peripheral)
        activePeripheral = nil
        pendingServices = 0
        pendingReads = 0
        writeStarted = false
        writeFinished = false
        inspectNext()
    }

    private func fail(_ message: String) {
        fputs("错误: \(message)\n", stderr)
        exit(1)
    }
}

guard let options = parseOptions() else {
    usage()
    exit(2)
}
if options.showHelp {
    usage()
    exit(0)
}

let inspector = BLEInspector(options: options)
RunLoop.main.run()
