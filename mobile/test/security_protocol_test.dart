import 'package:flutter_test/flutter_test.dart';
import 'package:smart_security/security_protocol.dart';

void main() {
  test('decodes signed temperature and unavailable sensor values', () {
    final state = SecurityState.decode([
      1,
      3,
      0xff,
      0xff,
      0x20,
      0x03,
      0xc9,
      0xff,
      0x20,
      0x02,
      0x94,
      0x27,
      0,
      0,
      0,
      0,
      0,
      0,
    ]);
    expect(state.armed, isTrue);
    expect(state.climateOnly, isFalse);
    expect(state.siren, isTrue);
    expect(state.distance, isNull);
    expect(state.light, 800);
    expect(state.temperature, -5.5);
    expect(state.humidity, 54.4);
    expect(state.pressure, 1013.2);
    expect(state.alertsSince(null), isEmpty);
  });

  test('counts a transient alert once even if the condition has cleared', () {
    final packet = List<int>.filled(18, 0)..[0] = protocolVersion;
    final initial = SecurityState.decode(packet);
    packet[12] = 1;
    final next = SecurityState.decode(packet);
    expect(next.isActive(SecurityAlert.distance), isFalse);
    expect(next.alertsSince(initial), [SecurityAlert.distance]);
    expect(next.alertsSince(next), isEmpty);
  });

  test('climate role remains known even when all sensors are unavailable', () {
    final packet = List<int>.filled(18, 0xff);
    packet[0] = protocolVersion;
    packet[1] = 1 << 5;
    packet[6] = 0;
    packet[7] = 0x80;
    final state = SecurityState.decode(packet);
    expect(state.climateOnly, isTrue);
    expect(state.armed, isFalse);
    expect(state.temperature, isNull);
    expect(state.humidity, isNull);
    expect(state.pressure, isNull);
    expect(state.alertsSince(null), isEmpty);
  });

  test('rejects unknown packet versions and sends versioned commands', () {
    expect(() => SecurityState.decode([]), throwsFormatException);
    expect(
      () => SecurityState.decode(List<int>.filled(18, 0)),
      throwsFormatException,
    );
    expect(SecurityCommand.arm.packet, [1, 2]);
    expect(SecurityCommand.sirenOff.packet, [1, 3]);
  });
}
