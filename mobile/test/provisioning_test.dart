import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:smart_security/provisioning.dart';
import 'package:smart_security/cloud_api.dart';

void main() {
  test('UTF-8 settings survive 20-byte BLE frames with contiguous offsets', () {
    final config = {
      'ssid': 'Дом 2.4',
      'password': 'пароль12345',
      'url': 'https://home.example.ru:8443',
      'id': 'a' * 36,
      'token': 'b' * 64,
    };
    final frames = provisioningFrames(config);
    expect(frames.first, [1, 0]);
    expect(frames.last, [1, 2]);
    final payload = <int>[];
    for (final frame in frames.skip(1).take(frames.length - 2)) {
      expect(frame.length, lessThanOrEqualTo(20));
      expect(frame[2] | frame[3] << 8, payload.length);
      payload.addAll(frame.skip(4));
    }
    expect(jsonDecode(utf8.decode(payload)), config);
  });
  test('Wi-Fi validation uses byte lengths and supports open networks', () {
    expect(
      () => provisioningFrames({'ssid': 'я' * 17, 'password': '12345678'}),
      throwsFormatException,
    );
    expect(
      () => provisioningFrames({'ssid': 'home', 'password': 'short'}),
      throwsFormatException,
    );
    expect(provisioningFrames({'ssid': 'home', 'password': ''}), isNotEmpty);
  });
  test('cloud endpoint requires HTTPS and accepts an explicit port', () {
    expect(
      CloudSession.validateUrl('https://home.example.ru:8443/'),
      'https://home.example.ru:8443',
    );
    for (final value in [
      'http://home.example.ru',
      'https://user:pass@home.example.ru',
      'https://home.example.ru/api',
      'https://home.example.ru?key=x',
    ]) {
      expect(() => CloudSession.validateUrl(value), throwsFormatException);
    }
  });
  test('network status distinguishes Wi-Fi from reachable cloud', () {
    final offline = NetworkStatus([1, 3, 12, 2]);
    expect(offline.configured, isTrue);
    expect(offline.wifi, isTrue);
    expect(offline.cloud, isFalse);
    expect(offline.queued, 12);
    expect(offline.lost, 2);
    expect(NetworkStatus([1, 15, 0, 0]).storageError, isTrue);
    expect(() => NetworkStatus([0, 0]), throwsFormatException);
  });
}
