import 'dart:convert';

const configUuid = 'd91fc004-cc92-4e99-9c67-2f7396311bce';
const networkUuid = 'd91fc005-cc92-4e99-9c67-2f7396311bce';

List<List<int>> provisioningFrames(Map<String, String> config) {
  final ssid = utf8.encode(config['ssid'] ?? '');
  final password = utf8.encode(config['password'] ?? '');
  if (ssid.isEmpty || ssid.length > 32) {
    throw const FormatException(
      'Название Wi-Fi должно занимать от 1 до 32 байт UTF-8.',
    );
  }
  if (password.isNotEmpty && (password.length < 8 || password.length > 63)) {
    throw const FormatException(
      'Пароль Wi-Fi — от 8 до 63 байт. Для открытой сети оставь пустым.',
    );
  }
  final payload = utf8.encode(jsonEncode(config));
  if (payload.length >= 1024) {
    throw const FormatException('Настройки слишком длинные.');
  }
  return [
    [1, 0],
    for (var offset = 0; offset < payload.length; offset += 16)
      [1, 1, offset & 255, offset >> 8, ...payload.skip(offset).take(16)],
    [1, 2],
  ];
}

class NetworkStatus {
  NetworkStatus(List<int> bytes) {
    if (bytes.length != 4 || bytes[0] != 1) {
      throw const FormatException('Обнови прошивку ESP32.');
    }
    configured = bytes[1] & 1 != 0;
    wifi = bytes[1] & 2 != 0;
    cloud = bytes[1] & 4 != 0;
    storageError = bytes[1] & 8 != 0;
    queued = bytes[2];
    lost = bytes[3];
  }
  late final bool configured, wifi, cloud, storageError;
  late final int queued, lost;
}
