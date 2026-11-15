import 'dart:async';

import 'package:material_ui/material_ui.dart';

import 'ble_security.dart';
import 'cloud_api.dart';
import 'cloud_page.dart';
import 'provisioning.dart';

class WifiPage extends StatefulWidget {
  const WifiPage({required this.ble, super.key});
  final BleSecurity ble;
  @override
  State<WifiPage> createState() => _WifiPageState();
}

class _WifiPageState extends State<WifiPage> {
  final ssid = TextEditingController();
  final password = TextEditingController();
  final name = TextEditingController();
  bool busy = false, linkCloud = false;
  String? message, selectedId;
  String? networkError;
  List<dynamic> devices = [];
  Map<String, String>? credentials;
  NetworkStatus? status;
  Timer? timer;
  bool checking = false;

  @override
  void initState() {
    super.initState();
    name.text = widget.ble.device?.name ?? 'ESP32';
    unawaited(checkStatus());
    timer = Timer.periodic(const Duration(seconds: 3), (_) => checkStatus());
  }

  @override
  void dispose() {
    timer?.cancel();
    ssid.dispose();
    password.dispose();
    name.dispose();
    super.dispose();
  }

  Future<void> checkStatus() async {
    if (busy || checking) return;
    if (!widget.ble.ready) {
      if (mounted) {
        setState(() {
          status = null;
          networkError =
              'Соединение по Bluetooth прервано. Подключись к ESP32 заново.';
        });
      }
      return;
    }
    checking = true;
    try {
      final result = await widget.ble.networkStatus();
      if (mounted) {
        setState(() {
          status = result;
          networkError = null;
        });
      }
    } catch (error) {
      debugPrint('BLE network status read failed: $error');
      if (mounted) {
        setState(() {
          status = null;
          networkError = error is TimeoutException
              ? 'ESP32 не ответила на запрос статуса сети. Проверь Bluetooth-связь и повтори.'
              : 'Не удалось прочитать статус сети по Bluetooth. После перепрошивки переподключись к плате; при необходимости забудь ESP32 в настройках Bluetooth телефона и выполни сопряжение заново.';
        });
      }
    } finally {
      checking = false;
    }
  }

  Future<void> loadDevices() async {
    try {
      final result = await cloudSession.request('GET', '/api/devices');
      if (mounted) {
        setState(() {
          final role = widget.ble.state?.climateOnly == true
              ? 'CLIMATE'
              : 'SECURITY';
          devices = (result as List).where((d) => d['role'] == role).toList();
        });
      }
    } catch (e) {
      if (mounted) setState(() => message = cloudError(e));
    }
  }

  Future<void> save() async {
    setState(() {
      busy = true;
      message = null;
    });
    bool bleStage = false;
    try {
      if (!widget.ble.ready) {
        throw const CloudException(
          'Сначала восстанови подключение к ESP32 по Bluetooth.',
        );
      }
      final config = <String, String>{
        'ssid': ssid.text,
        'password': password.text,
      };
      provisioningFrames(
        config,
      ); // Validate before creating any cloud identity.
      if (linkCloud) {
        if (!cloudSession.signedIn) {
          throw const CloudException('Сначала войди в облако.');
        }
        if (credentials == null) {
          final dynamic result;
          if (selectedId == null) {
            if (name.text.trim().isEmpty) {
              throw const FormatException('Введи имя устройства.');
            }
            result = await cloudSession.request('POST', '/api/devices', {
              'name': name.text.trim(),
              'role': widget.ble.state?.climateOnly == true
                  ? 'CLIMATE'
                  : 'SECURITY',
            });
          } else {
            result = await cloudSession.request(
              'POST',
              '/api/devices/$selectedId/token',
              {},
            );
          }
          credentials = {
            'id': result['id'] as String,
            'token': result['token'] as String,
            'url': cloudSession.baseUrl,
          };
        }
        config.addAll(credentials!);
      }
      bleStage = true;
      await widget.ble.configureNetwork(config);
      if (mounted) {
        password.clear();
        setState(
          () => message =
              'Настройки сохранены на ESP32. Ожидаем подключения к сети.',
        );
      }
    } catch (e) {
      if (mounted) {
        setState(
          () => message = e is FormatException
              ? e.message
              : bleStage
              ? 'ESP32 не подтвердила настройки. Проверь BLE-связь и прошивку, затем повтори.'
              : cloudError(e),
        );
      }
    } finally {
      if (mounted) {
        setState(() => busy = false);
        unawaited(checkStatus());
      }
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Wi-Fi выбранной ESP32')),
    body: ListView(
      padding: const EdgeInsets.all(16),
      children: [
        const Text(
          'Сеть 2,4 ГГц. Настройки передаются по защищённому Bluetooth и сохраняются на плате.',
        ),
        if (status != null)
          Card(
            child: ListTile(
              leading: Icon(status!.wifi ? Icons.wifi : Icons.wifi_off),
              title: Text(
                status!.wifi
                    ? 'Wi-Fi подключён'
                    : status!.configured
                    ? 'Подключаемся к Wi-Fi'
                    : 'Wi-Fi ещё не настроен',
              ),
              subtitle: Text(
                '${status!.cloud ? 'Облако на связи' : 'Нет связи с облаком'} · тревог в очереди: ${status!.queued}',
              ),
            ),
          ),
        if (status?.storageError == true)
          const Text(
            'Ошибка памяти или запуска сетевого модуля ESP32. Проверь Serial Monitor и таблицу разделов. Тревоги могут не сохраниться.',
          ),
        if ((status?.lost ?? 0) > 0)
          Text('Переполнение очереди, потеряно тревог: ${status!.lost}'),
        TextField(
          controller: ssid,
          enabled: !busy,
          autocorrect: false,
          decoration: const InputDecoration(labelText: 'Название Wi-Fi (SSID)'),
        ),
        TextField(
          controller: password,
          enabled: !busy,
          obscureText: true,
          decoration: const InputDecoration(labelText: 'Пароль Wi-Fi'),
        ),
        const SizedBox(height: 16),
        SwitchListTile(
          title: const Text('Привязать к облачному аккаунту'),
          value: linkCloud,
          onChanged: busy
              ? null
              : (value) async {
                  setState(() {
                    linkCloud = value;
                  });
                  if (value && cloudSession.signedIn) await loadDevices();
                },
        ),
        if (!linkCloud)
          const Text(
            'Можно изменить только Wi-Fi, даже если облако недоступно. Существующая привязка к аккаунту сохранится.',
          ),
        if (linkCloud && !cloudSession.signedIn)
          TextButton(
            onPressed: busy
                ? null
                : () async {
                    await Navigator.of(context).push(
                      MaterialPageRoute<void>(
                        builder: (_) => const CloudPage(),
                      ),
                    );
                    if (mounted) {
                      setState(() {});
                      if (cloudSession.signedIn) await loadDevices();
                    }
                  },
            child: const Text('Войти в облако'),
          ),
        if (linkCloud && cloudSession.signedIn) ...[
          DropdownButtonFormField<String>(
            initialValue: selectedId ?? '',
            decoration: const InputDecoration(
              labelText: 'Устройство в аккаунте',
            ),
            items: [
              const DropdownMenuItem(
                value: '',
                child: Text('Добавить новое устройство'),
              ),
              for (final d in devices)
                DropdownMenuItem(
                  value: d['id'] as String,
                  child: Text(d['name'] as String),
                ),
            ],
            onChanged: busy
                ? null
                : (v) => setState(() {
                    selectedId = v == '' ? null : v;
                    credentials = null;
                  }),
          ),
          if (selectedId == null)
            TextField(
              controller: name,
              enabled: !busy,
              maxLength: 80,
              decoration: const InputDecoration(labelText: 'Имя устройства'),
            ),
          if (selectedId != null)
            const Text(
              'Будет заменён ключ подключения выбранного устройства. Его история сохранится.',
            ),
        ],
        const SizedBox(height: 16),
        FilledButton(
          onPressed: busy ? null : save,
          child: const Text('Сохранить на ESP32'),
        ),
        if (busy) const LinearProgressIndicator(),
        if (message != null)
          Padding(padding: const EdgeInsets.all(12), child: Text(message!)),
        if (networkError != null) ...[
          Padding(
            padding: const EdgeInsets.all(12),
            child: Text(networkError!),
          ),
          TextButton(
            onPressed: busy || checking ? null : checkStatus,
            child: const Text('Повторить чтение статуса'),
          ),
        ],
      ],
    ),
  );
}
