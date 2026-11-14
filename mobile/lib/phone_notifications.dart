import 'dart:io';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'security_protocol.dart';

class PhoneNotifications {
  final _plugin = FlutterLocalNotificationsPlugin();
  Future<bool?>? _initialization;

  Future<bool?> _initialize() => _initialization ??= _plugin.initialize(
    settings: const InitializationSettings(
      android: AndroidInitializationSettings('ic_security_alert'),
      iOS: DarwinInitializationSettings(
        requestAlertPermission: false,
        requestBadgePermission: false,
        requestSoundPermission: false,
      ),
    ),
  );

  Future<bool> requestPermission() async {
    await _initialize();
    if (Platform.isAndroid) {
      return await _plugin
              .resolvePlatformSpecificImplementation<
                AndroidFlutterLocalNotificationsPlugin
              >()
              ?.requestNotificationsPermission() ??
          false;
    }
    return await _plugin
            .resolvePlatformSpecificImplementation<
              IOSFlutterLocalNotificationsPlugin
            >()
            ?.requestPermissions(alert: true, sound: true) ??
        false;
  }

  Future<void> show(SecurityAlert alert) async {
    await _initialize();
    await _plugin.show(
      id: alert.index + 1,
      title: 'Охрана ESP32',
      body: alert.title,
      notificationDetails: const NotificationDetails(
        android: AndroidNotificationDetails(
          'security_alerts',
          'События охраны',
          importance: Importance.high,
          priority: Priority.high,
        ),
        iOS: DarwinNotificationDetails(),
      ),
    );
  }
}
