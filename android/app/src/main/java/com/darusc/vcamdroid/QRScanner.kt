package com.darusc.vcamdroid

import android.graphics.Rect
import android.util.Log
import androidx.annotation.OptIn
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ImageProxy
import com.google.mlkit.vision.barcode.BarcodeScanner
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage

class QRScanner() {

    data class Result(
        var address: String,
        var port: Int,
        var pairingToken: String
    )

    private var options: BarcodeScannerOptions
    private var scanner: BarcodeScanner

    private var addressRegex = Regex("""^(25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\.(25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\.(25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\.(25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})$""")

    private var enabled = false

    init {
        options = BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        scanner = BarcodeScanning.getClient(options)
    }

    fun start() {
        enabled = true
    }

    fun stop() {
        enabled = false
    }

    @OptIn(ExperimentalGetImage::class)
    fun launchScanTask(imageProxy: ImageProxy, rect: Rect, callback: (QRScanner.Result?) -> Unit) {
        if(!enabled) {
            imageProxy.close()
            return
        }

        val mediaImage = imageProxy.image
        if(mediaImage != null) {
            val image = InputImage.fromMediaImage(mediaImage, imageProxy.imageInfo.rotationDegrees)
            val result = scanner.process(image)
                .addOnSuccessListener { barcodes ->
                    for (barcode in barcodes) {
                        // Only capture QR codes that are inside the given rectangle area
                        barcode.boundingBox?.let {
                            if(!rect.contains(it)) {
                                imageProxy.close()
                                return@addOnSuccessListener
                            }
                        }
                        // Stop the scanner. Further attempts at connecting should be
                        // manually triggered otherwise multiple connection might be established
                        // (for each frame of the camera)
                        stop()
                        callback(parseResult(barcode.rawValue ?: ""))
                    }
                    imageProxy.close()
                }
                .addOnFailureListener {
                    imageProxy.close()
                }
        }
    }

    private fun parseResult(value: String): Result? {
        val splits = value.split('|')
        if (splits.size != 4 || splits[0] != "OCAM1") return null
        if (!splits[1].matches(addressRegex)) return null
        val port = splits[2].toIntOrNull() ?: return null
        if (port !in 1..65535) return null
        if (splits[3].length != 64 || splits[3].any { it !in "0123456789abcdefABCDEF" }) return null
        return Result(splits[1], port, splits[3])
    }
}
