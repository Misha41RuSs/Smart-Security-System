import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import 'phone_notifications.dart';
import 'security_protocol.dart';
import 'provisioning.dart';

typedef AlertEntry = ({SecurityAlert alert, DateTime time});

class BleSecurity extends ChangeNotifier {
  late final _ble = FlutterReactiveBle();
  final _notifications = PhoneNotifications();
  static const _permissions = MethodChannel('smart_security/ble');
  final _serviceId = Uuid.parse(serviceUuid);
  final _commandId = Uuid.parse(commandUuid);
  final _stateId = Uuid.parse(stateUuid);
  final _devices = <String, DiscoveredDevice>{};
  final _events = <AlertEntry>[];

  StreamSubscription<DiscoveredDevice>? _scan;
  StreamSubscription<ConnectionStateUpdate>? _connection;
  StreamSubscription<List<int>>? _updates;
  Timer? _scanTimeout;
  Timer? _updateTimeout;
  int _session = 0;
  bool _disposed = false;
  bool _scanning = false;
  bool _ready = false;
  bool _writing = false;
  bool _endingConnection = false;
  bool _settingNotifications = false;
  String? _notificationStatus;
  String? _message;
  DiscoveredDevice? _device;
  SecurityState? _state;

  Iterable<DiscoveredDevice> get devices => _devices.values;
  List<AlertEntry> get events => List.unmodifiable(_events);
  SecurityState? get state => _ready ? _state : null;
  DiscoveredDevice? get device => _device;
  bool get scanning => _scanning;
  bool get ready => _ready;
  bool get writing => _writing;
  bool get settingNotifications => _settingNotifications;
  String? get notificationStatus => _notificationStatus;
  String? get message => _message;

  Future<void> scan() async {
    if (_scanning || _device != null) return;
    _devices.clear();
    _message = null;
    _scanning = true;
    notifyListeners();

    try {
      if (Platform.isAndroid &&
          await _permissions.invokeMethod<bool>('requestPermissions') != true) {
        throw StateError('Разреши доступ к Bluetooth в настройках телефона.');
      }
      var status = _ble.status;
      if (status != BleStatus.ready) {
        status = await _ble.statusStream
            .firstWhere((value) => value == BleStatus.ready)
            .timeout(const Duration(seconds: 12), onTimeout: () => _ble.status);
      }
      if (_disposed || !_scanning) return;
      if (status != BleStatus.ready) {
        throw StateError(switch (status) {
          BleStatus.unauthorized =>
            'Разреши доступ к Bluetooth в настройках телефона.',
          BleStatus.poweredOff => 'Включи Bluetooth на телефоне.',
          BleStatus.locationServicesDisabled =>
            'Для поиска BLE на этой версии Android включи геолокацию.',
          BleStatus.unsupported => 'На устройстве недоступен Bluetooth LE.',
          _ => 'Bluetooth пока недоступен. Повтори поиск.',
        });
      }
      _scan = _ble
          .scanForDevices(
            withServices: [_serviceId],
            scanMode: ScanMode.lowLatency,
          )
          .listen(
            (device) {
              _devices[device.id] = device;
              notifyListeners();
            },
            onError: (Object error) {
              _message = 'Ошибка поиска BLE. Проверь разрешения и Bluetooth.';
              unawaited(_stopScan());
            },
            onDone: () => unawaited(_stopScan()),
          );
      _scanTimeout = Timer(const Duration(seconds: 10), () {
        if (_devices.isEmpty) {
          _message = 'Нет устройств поблизости. Проверь питание ESP32.';
        }
        unawaited(_stopScan());
      });
    } on StateError catch (error) {
      _message = error.message.toString();
      await _stopScan();
    } catch (_) {
      _message = 'Не удалось запустить поиск Bluetooth.';
      await _stopScan();
    }
  }

  Future<void> _stopScan() async {
    _scanTimeout?.cancel();
    _scanTimeout = null;
    final scan = _scan;
    _scan = null;
    _scanning = false;
    await scan?.cancel();
    if (!_disposed) notifyListeners();
  }

  Future<void> connect(DiscoveredDevice device) async {
    if (_device != null) return;
    final session = ++_session;
    _device = device;
    _events.clear();
    _message = null;
    notifyListeners();
    await _stopScan();
    if (!_isCurrent(session)) return;
    _connection = _ble
        .connectToDevice(
          id: device.id,
          connectionTimeout: const Duration(seconds: 15),
        )
        .listen(
          (update) {
            if (!_isCurrent(session)) return;
            if (update.connectionState == DeviceConnectionState.connected) {
              unawaited(_subscribe(device.id, session));
            } else if (update.connectionState ==
                DeviceConnectionState.disconnected) {
              unawaited(_endConnection('Связь с ESP32 прервана.'));
            }
          },
          onError: (Object error) {
            if (_isCurrent(session)) {
              unawaited(_endConnection('Не удалось подключиться к ESP32.'));
            }
          },
          onDone: () {
            if (_isCurrent(session)) {
              unawaited(_endConnection('Связь с ESP32 прервана.'));
            }
          },
        );
  }

  bool _isCurrent(int session) => !_disposed && _session == session;

  Future<void> _subscribe(String id, int session) async {
    final characteristic = QualifiedCharacteristic(
      deviceId: id,
      serviceId: _serviceId,
      characteristicId: _stateId,
    );
    final deadline = DateTime.now().add(const Duration(seconds: 50));
    List<int>? initial;
    while (_isCurrent(session) && DateTime.now().isBefore(deadline)) {
      try {
        initial = await _ble
            .readCharacteristic(characteristic)
            .timeout(deadline.difference(DateTime.now()));
        break;
      } catch (_) {
        // A protected read can fail while the system pairing dialog is open.
        await Future<void>.delayed(const Duration(seconds: 1));
      }
    }
    if (!_isCurrent(session)) return;
    if (initial == null) {
      await _endConnection(
        'Не удалось завершить сопряжение. Переподключись и введи PIN из Serial.',
      );
      return;
    }

    try {
      _receive(initial);
    } on FormatException catch (error) {
      await _endConnection(error.message);
      return;
    }

    _updateTimeout = Timer(const Duration(seconds: 8), () {
      if (_isCurrent(session) && !_ready) {
        unawaited(
          _endConnection('ESP32 не подтвердила подписку на показания.'),
        );
      }
    });
    _updates = _ble
        .subscribeToCharacteristic(characteristic)
        .listen(
          (packet) {
            if (!_isCurrent(session)) return;
            try {
              _receive(packet);
              _updateTimeout?.cancel();
              _ready = true;
              notifyListeners();
            } on FormatException catch (error) {
              unawaited(_endConnection(error.message));
            }
          },
          onError: (Object error) {
            if (_isCurrent(session)) {
              unawaited(
                _endConnection('Не удалось подписаться на показания ESP32.'),
              );
            }
          },
          onDone: () {
            if (_isCurrent(session)) {
              unawaited(_endConnection('Поток показаний ESP32 закрыт.'));
            }
          },
        );
  }

  void _receive(List<int> packet) {
    final next = SecurityState.decode(packet);
    for (final alert in next.alertsSince(_state)) {
      _events.insert(0, (alert: alert, time: DateTime.now()));
      if (_events.length > 30) _events.removeLast();
      unawaited(_showAlert(alert));
    }
    _state = next;
  }

  Future<void> _showAlert(SecurityAlert alert) async {
    try {
      await _notifications.show(alert);
    } catch (error) {
      debugPrint('Local notification failed: $error');
    }
  }

  Future<void> requestNotifications() async {
    if (_settingNotifications) return;
    _settingNotifications = true;
    notifyListeners();
    try {
      final allowed = await _notifications.requestPermission();
      _notificationStatus = allowed
          ? 'Уведомления разрешены'
          : 'Разрешить их можно в настройках телефона';
    } catch (_) {
      _notificationStatus = 'Не удалось проверить разрешение';
    } finally {
      _settingNotifications = false;
      if (!_disposed) notifyListeners();
    }
  }

  Future<void> send(SecurityCommand command) async {
    final device = _device;
    if (!_ready || _writing || device == null || _state?.climateOnly == true) {
      return;
    }
    final session = _session;
    _writing = true;
    _message = null;
    notifyListeners();
    try {
      await _ble
          .writeCharacteristicWithResponse(
            QualifiedCharacteristic(
              deviceId: device.id,
              serviceId: _serviceId,
              characteristicId: _commandId,
            ),
            value: command.packet,
          )
          .timeout(const Duration(seconds: 8));
    } catch (_) {
      if (_isCurrent(session)) {
        _message = 'ESP32 не подтвердила команду. Проверь связь.';
      }
    } finally {
      if (_isCurrent(session)) {
        _writing = false;
        notifyListeners();
      }
    }
  }

  Future<NetworkStatus> networkStatus() async {
    if (!_ready || _device == null) throw StateError('Нет BLE-соединения');
    final bytes = await _ble
        .readCharacteristic(
          QualifiedCharacteristic(
            deviceId: _device!.id,
            serviceId: _serviceId,
            characteristicId: Uuid.parse(networkUuid),
          ),
        )
        .timeout(const Duration(seconds: 8));
    return NetworkStatus(bytes);
  }

  Future<void> configureNetwork(Map<String, String> config) async {
    if (!_ready || _device == null || _writing) {
      throw StateError('Нет свободного BLE-соединения');
    }
    final frames = provisioningFrames(config);
    final session = _session;
    final characteristic = QualifiedCharacteristic(
      deviceId: _device!.id,
      serviceId: _serviceId,
      characteristicId: Uuid.parse(configUuid),
    );
    _writing = true;
    notifyListeners();
    try {
      for (final frame in frames) {
        if (!_isCurrent(session)) throw StateError('BLE-соединение прервано');
        await _ble
            .writeCharacteristicWithResponse(characteristic, value: frame)
            .timeout(const Duration(seconds: 8));
      }
    } finally {
      if (_isCurrent(session)) {
        _writing = false;
        notifyListeners();
      }
    }
  }

  Future<void> disconnect() => _endConnection(null);

  Future<void> _endConnection(String? message) async {
    if (_endingConnection) return;
    _endingConnection = true;
    final session = ++_session;
    _updateTimeout?.cancel();
    final updates = _updates;
    final connection = _connection;
    _updates = null;
    _connection = null;
    _ready = false;
    _writing = false;
    _state = null;
    try {
      await updates?.cancel();
    } catch (error) {
      debugPrint('BLE subscription cleanup failed: $error');
    }
    try {
      await connection?.cancel();
    } catch (error) {
      debugPrint('BLE connection cleanup failed: $error');
    } finally {
      _endingConnection = false;
      if (_isCurrent(session)) {
        _device = null;
        _message = message;
        notifyListeners();
      }
    }
  }

  @override
  void dispose() {
    _disposed = true;
    unawaited(_stopScan());
    unawaited(_endConnection(null));
    super.dispose();
  }
}
