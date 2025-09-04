#!/usr/bin/env node

const fs = require('fs');
const path = require('path');
const FormData = require('form-data');
const fetch = require('node-fetch');

/**
 * ESP32 ET112 Proxy Device Uploader
 * 
 * Uploads firmware or filesystem images to ESP32 devices via HTTP POST.
 * Useful for upgrading devices with broken or missing web interfaces.
 * 
 * Usage:
 *   node upload_device.js <type> <ip> <filename>
 *   
 * Types:
 *   firmware   - Upload firmware to /update endpoint
 *   filesystem - Upload filesystem to /upload-filesystem endpoint
 * 
 * Examples:
 *   node upload_device.js firmware 192.168.1.100 firmware.bin
 *   node upload_device.js filesystem 192.168.1.100 littlefs.bin
 */

function showUsage() {
    console.log('Usage: node upload_device.js <type> <ip> <filename>');
    console.log('');
    console.log('Types:');
    console.log('  firmware   - Upload firmware to /update endpoint');
    console.log('  filesystem - Upload filesystem to /upload-filesystem endpoint');
    console.log('');
    console.log('Examples:');
    console.log('  node upload_device.js firmware 192.168.1.100 firmware.bin');
    console.log('  node upload_device.js filesystem 192.168.1.100 littlefs.bin');
}

async function uploadFile(type, ip, filename) {
    // Validate arguments
    if (!type || !ip || !filename) {
        console.error('Error: Missing required arguments');
        showUsage();
        process.exit(1);
    }

    // Validate type
    if (type !== 'firmware' && type !== 'filesystem') {
        console.error('Error: Invalid type. Must be "firmware" or "filesystem"');
        showUsage();
        process.exit(1);
    }

    // Check if file exists
    if (!fs.existsSync(filename)) {
        console.error(`Error: File "${filename}" not found`);
        process.exit(1);
    }

    // Get file stats
    const stats = fs.statSync(filename);
    const fileSize = stats.size;
    const filePath = path.resolve(filename);
    const baseName = path.basename(filename);

    console.log(`Uploading ${type}: ${baseName} (${fileSize} bytes) to ${ip}`);

    // Determine endpoint
    const endpoint = type === 'firmware' ? '/update' : '/upload-filesystem';
    const url = `http://${ip}${endpoint}`;

    try {
        // Create form data
        const form = new FormData();
        const fileStream = fs.createReadStream(filePath);
        
        // The ESP32 expects a file upload with proper content type
        // Use the original filename but ensure proper content type
        const options = {
            filename: baseName,
            contentType: 'application/octet-stream',
            knownLength: fileSize
        };
        
        form.append('file', fileStream, options);

        // Set up progress tracking
        let uploaded = 0;
        let lastProgress = -1;

        fileStream.on('data', (chunk) => {
            uploaded += chunk.length;
            const progress = Math.floor((uploaded / fileSize) * 100);
            
            // Only log progress every 5%
            if (progress >= lastProgress + 5) {
                process.stdout.write(`\rProgress: ${progress}%`);
                lastProgress = progress;
            }
        });

        console.log(`Connecting to ${url}...`);

        // Upload file
        
        const response = await fetch(url, {
            method: 'POST',
            body: form,
            // Remove timeout as it's not a standard fetch option
            headers: {
                ...form.getHeaders()
            }
        });

        process.stdout.write('\n'); // New line after progress

        if (!response.ok) {
            const errorText = await response.text();
            console.error(`Upload failed: HTTP ${response.status} ${response.statusText}`);
            console.error('Response:', errorText);
            process.exit(1);
        }

        // Handle response based on content type
        const contentType = response.headers.get('content-type') || '';
        
        if (contentType.includes('application/json')) {
            const result = await response.json();
            console.log('Upload completed successfully!');
            console.log('Response:', result);
            
            if (result.reboot) {
                console.log('\n⚠️  Device is rebooting - please wait 30-60 seconds before reconnecting');
            }
        } else {
            const text = await response.text();
            console.log('Upload completed successfully!');
            
            // For filesystem uploads, the response is HTML with countdown
            if (type === 'filesystem' && text.includes('Rebooting')) {
                console.log('\n⚠️  Device is rebooting - please wait 45-60 seconds before accessing the web interface');
            }
        }

    } catch (error) {
        process.stdout.write('\n'); // New line after progress
        
        if (error.code === 'ECONNREFUSED') {
            console.error(`Error: Could not connect to device at ${ip}`);
            console.error('Make sure the device is powered on and connected to the network');
        } else if (error.code === 'ENOTFOUND') {
            console.error(`Error: Could not resolve hostname ${ip}`);
        } else if (error.type === 'request-timeout') {
            console.error('Error: Upload timed out (device may still be processing)');
        } else {
            console.error('Upload failed:', error.message);
        }
        process.exit(1);
    }
}

// Parse command line arguments
const args = process.argv.slice(2);

if (args.length === 0 || args[0] === '--help' || args[0] === '-h') {
    showUsage();
    process.exit(0);
}

const [type, ip, filename] = args;

// Run the upload
uploadFile(type, ip, filename).catch(error => {
    console.error('Unexpected error:', error);
    process.exit(1);
});