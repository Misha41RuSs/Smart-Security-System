import 'dart:typed_data';

const serviceUuid = 'd91fc001-cc92-4e99-9c67-2f7396311bce';
const commandUuid = 'd91fc002-cc92-4e99-9c67-2f7396311bce';
const stateUuid = 'd91fc003-cc92-4e99-9c67-2f7396311bce';
const protocolVersion = 1;

enum SecurityCommand {
  disarm(1),
  arm(2),
  sirenOff(3),
  sirenOn(4);

  const SecurityCommand(this.code);
  final int code;

  List<int> get packet => [protocolVersion, code];
}

enum SecurityAlert {
  distance(1 << 2, 12, 'Расстояние ниже порога'),
  light(1 << 3, 14, 'Обнаружен свет'),
  temperature(1 << 4, 16, 'Температура выше порога');

  const SecurityAlert(this.flag, this.counterOffset, this.title);
  final int flag;
  final int counterOffset;
  final String title;
}

class SecurityState {
  SecurityState.decode(List<int> packet)
    : _data = ByteData.sublistView(Uint8List.fromList(packet)) {
    if (_data.lengthInBytes != 18 || _data.getUint8(0) != protocolVersion) {
      throw const FormatException('Несовместимая версия BLE-протокола');
    }
  }

  final ByteData _data;

  int get _flags => _data.getUint8(1);
  bool get climateOnly => _flags & (1 << 5) != 0;
  bool get armed => _flags & (1 << 0) != 0;
  bool get siren => _flags & (1 << 1) != 0;
  bool isActive(SecurityAlert alert) => _flags & alert.flag != 0;

  double? get distance => _tenths(2);
  int? get light => _unsigned(4);
  double? get temperature {
    final raw = _data.getInt16(6, Endian.little);
    return raw == -32768 ? null : raw / 10;
  }

  double? get humidity => _tenths(8);
  double? get pressure => _tenths(10);
  int counter(SecurityAlert alert) =>
      _data.getUint16(alert.counterOffset, Endian.little);

  List<SecurityAlert> alertsSince(SecurityState? previous) => [
    for (final alert in SecurityAlert.values)
      if (previous == null
          ? isActive(alert)
          : counter(alert) != previous.counter(alert))
        alert,
  ];

  int? _unsigned(int offset) {
    final raw = _data.getUint16(offset, Endian.little);
    return raw == 0xffff ? null : raw;
  }

  double? _tenths(int offset) {
    final raw = _unsigned(offset);
    return raw == null ? null : raw / 10;
  }
}
