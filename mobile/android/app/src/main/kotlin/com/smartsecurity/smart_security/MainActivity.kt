package com.smartsecurity.smart_security

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var pendingPermission: MethodChannel.Result? = null
    private val permissionRequest = 812

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "smart_security/ble")
            .setMethodCallHandler { call, result ->
                if (call.method != "requestPermissions") {
                    result.notImplemented()
                    return@setMethodCallHandler
                }
                val permissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
                } else {
                    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
                }
                val missing = permissions.filter {
                    checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
                }
                if (missing.isEmpty()) {
                    result.success(true)
                } else if (pendingPermission != null) {
                    result.error("permission_request_pending", null, null)
                } else {
                    pendingPermission = result
                    requestPermissions(missing.toTypedArray(), permissionRequest)
                }
            }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == permissionRequest) {
            pendingPermission?.success(
                grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }
            )
            pendingPermission = null
        }
    }
}
