package com.darusc.vcamdroid

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Rect
import android.net.Uri
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.provider.Settings
import android.util.Size
import android.widget.Toast
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AlertDialog
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.view.PreviewView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.darusc.vcamdroid.databinding.ActivityMainBinding
import com.darusc.vcamdroid.networking.ConnectionManager
import com.darusc.vcamdroid.util.Logger
import com.darusc.vcamdroid.video.Camera
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class MainActivity : AppCompatActivity(), ConnectionManager.ConnectionStateCallback {

    private lateinit var viewBinding: ActivityMainBinding

    private val qrscanner = QRScanner()
    private var connectionManager = ConnectionManager.getInstance(this)
    private var camera: Camera? = null

    private var isConnecting = false
    private var selectedMode = ConnectionManager.ConnectionMode.WIFI
    private val modePreferences by lazy {
        getSharedPreferences("connection_preferences", MODE_PRIVATE)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        viewBinding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(viewBinding.root)

        viewBinding.logReportButton.setOnClickListener {
            val intent = Intent(this, LogActivity::class.java)
            startActivity(intent)
        }

        enableEdgeToEdge()
        selectedMode = modePreferences.getString("mode", null)
            ?.let { runCatching { ConnectionManager.ConnectionMode.valueOf(it) }.getOrNull() }
            ?: ConnectionManager.ConnectionMode.WIFI
        viewBinding.connectionModeGroup.check(
            if (selectedMode == ConnectionManager.ConnectionMode.WIFI)
                viewBinding.wifiModeButton.id else viewBinding.usbModeButton.id
        )
        viewBinding.connectionModeGroup.setOnCheckedChangeListener { _, checkedId ->
            val nextMode = if (checkedId == viewBinding.usbModeButton.id)
                ConnectionManager.ConnectionMode.USB else ConnectionManager.ConnectionMode.WIFI
            if (nextMode != selectedMode) {
                selectedMode = nextMode
                modePreferences.edit().putString("mode", nextMode.name).apply()
                isConnecting = false
                connectionManager.close()
                if (camera != null) startSelectedMode()
            }
        }
        initialize()
    }

    override fun onResume() {
        super.onResume()
        connectionManager = ConnectionManager.getInstance(this)
        if(camera != null) {
            camera!!.start(Size(1280, 720), CameraSelector.DEFAULT_BACK_CAMERA)
            startSelectedMode()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if(requestCode == 1000) {
            if(grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                initialize()
            } else {
                // If camera permission was not granted show a prompt to the user
                // to go to settings and enable the required permission

                MaterialAlertDialogBuilder(this)
                    .setTitle("Camera Permission Required")
                    .setMessage("Please enable camera permission in settings and restart the app.")
                    .setPositiveButton("Go to settings") { _, _ ->
                        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
                            data = Uri.fromParts("package", packageName, null)
                        }
                        startActivity(intent)
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
        }
    }

    override fun onConnectionSuccessful(connectionMode: ConnectionManager.ConnectionMode) {
        runOnUiThread {
            isConnecting = false
            qrscanner.stop()
            Logger.log("MAIN", "Connection successful $connectionMode")

            val intent = Intent(this, StreamActivity::class.java)
            startActivity(intent)
        }
    }

    override fun onConnectionFailed(connectionMode: ConnectionManager.ConnectionMode) {
        runOnUiThread {
            isConnecting = false
            Logger.log("MAIN", "Connection failed in explicitly selected $connectionMode mode")
            if (connectionMode == ConnectionManager.ConnectionMode.WIFI) {
                viewBinding.overlay.setInstruction("Wi-Fi selected: scan the current OpenCam QR to retry")
                qrscanner.start()
                Toast.makeText(this, "Wi-Fi pairing failed. Scan the current Linux QR again.", Toast.LENGTH_LONG).show()
            } else {
                viewBinding.overlay.setInstruction("USB / ADB selected: enable USB debugging and run the legacy Windows server")
                Toast.makeText(this, "USB/ADB connection failed. Check debugging and the Windows server.", Toast.LENGTH_LONG).show()
            }
        }
    }

    /**
     * Check for CAMERA permission and request it if not granted
     */
    private fun checkPermissions(): Boolean {
        if(ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), 1000)
            return false
        }

        return true
    }

    /**
     * If the required permissions are grante create the camera instance,
     * start it, and initialize the connection procedure
     */
    private fun initialize() {
        if(checkPermissions()) {
            camera = Camera(
                viewBinding.viewFinder.surfaceProvider,
                ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888,
                ::processImage,
                this,
                this
            )
            camera!!.start(Size(1280, 720), CameraSelector.DEFAULT_BACK_CAMERA)
            startSelectedMode()
        }
    }

    private fun processImage(imageProxy: ImageProxy) {
        // Process each incoming image from the camera
        // launchScanTask() will scan and call the callback on success
        // only if start() was called before
        qrscanner.launchScanTask(imageProxy, camera?.screenRectToImageRect(viewBinding.overlay.rect, viewBinding.overlay.size) ?: Rect()) { result ->
            runOnUiThread {
                if(result != null) {
                    qrscanner.stop()
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Connect via WiFi")
                        .setMessage("Pair with ${result.address}:${result.port}? Verify that this is your OpenCam host.")
                        .setPositiveButton("Connect") { _, _ ->
                            isConnecting = true
                            viewBinding.overlay.setInstruction("Wi-Fi selected: authenticating with ${result.address}:${result.port}…")
                            connectionManager.connect(result.address, result.port, result.pairingToken)
                        }
                        .setNegativeButton("Cancel") { _, _ -> startSelectedMode() }
                        .show()
                } else {
                    Logger.log("MAIN", "Invalid QR code")
                    Toast.makeText(this, "Invalid QR code", Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun startSelectedMode() {
        when (selectedMode) {
            ConnectionManager.ConnectionMode.WIFI -> {
                isConnecting = false
                viewBinding.overlay.setInstruction("Wi-Fi selected: scan the OpenCam pairing QR code")
                qrscanner.start()
            }
            ConnectionManager.ConnectionMode.USB -> {
                qrscanner.stop()
                viewBinding.overlay.setInstruction("USB / ADB selected: enable USB debugging and run the legacy Windows server")
                if (!isConnecting) {
                    isConnecting = true
                    connectionManager.connect(6969)
                }
            }
        }
    }
}
