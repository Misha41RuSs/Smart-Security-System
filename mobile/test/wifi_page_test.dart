import 'dart:async';

import 'package:material_ui/material_ui.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:smart_security/ble_security.dart';
import 'package:smart_security/provisioning.dart';
import 'package:smart_security/wifi_page.dart';

class TestBle extends BleSecurity {
  bool fail = true;
  @override
  bool get ready => true;
  @override
  Future<NetworkStatus> networkStatus() async {
    if (fail) throw TimeoutException('read timed out');
    return NetworkStatus([1, 3, 0, 0]);
  }
}

void main() {
  testWidgets('network read failure clears after successful retry', (
    tester,
  ) async {
    final ble = TestBle();
    await tester.pumpWidget(MaterialApp(home: WifiPage(ble: ble)));
    await tester.pump();
    expect(find.textContaining('не ответила на запрос'), findsOneWidget);
    expect(find.textContaining('нужна новая прошивка'), findsNothing);
    ble.fail = false;
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    expect(find.text('Wi-Fi подключён'), findsOneWidget);
    expect(find.textContaining('не ответила на запрос'), findsNothing);
    await tester.pumpWidget(const SizedBox());
    ble.dispose();
  });
}
