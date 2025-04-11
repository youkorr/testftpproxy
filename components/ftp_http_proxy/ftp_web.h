#ifndef FTP_WEB_H
#define FTP_WEB_H

#include <string>

namespace esphome {
namespace ftp_http_proxy {

// Page HTML pour l'interface web
const char FTP_WEB_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FTP File Manager</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 20px;
      background-color: #f4f4f9;
    }
    h1 {
      color: #333;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 20px;
    }
    th, td {
      padding: 10px;
      border: 1px solid #ddd;
      text-align: left;
    }
    th {
      background-color: #f0f0f0;
    }
    button {
      padding: 5px 10px;
      background-color: #007bff;
      color: white;
      border: none;
      cursor: pointer;
    }
    button:hover {
      background-color: #0056b3;
    }
    .delete-btn {
      background-color: #dc3545;
    }
    .delete-btn:hover {
      background-color: #a71d2a;
    }
    .upload-form {
      margin-top: 20px;
    }
  </style>
</head>
<body>
  <h1>FTP File Manager</h1>
  <div>
    <table id="fileTable">
      <thead>
        <tr>
          <th>File Name</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        <!-- Files will be dynamically populated here -->
      </tbody>
    </table>
  </div>
  <div class="upload-form">
    <h2>Upload File</h2>
    <form id="uploadForm" enctype="multipart/form-data">
      <input type="file" name="file" id="fileInput" required>
      <button type="submit">Upload</button>
    </form>
  </div>
  <script>
    // Fetch and display remote files
    async function fetchFiles() {
      const response = await fetch('/list');
      if (response.ok) {
        const files = await response.json();
        const tbody = document.querySelector('#fileTable tbody');
        tbody.innerHTML = ''; // Clear existing rows
        files.forEach(file => {
          const row = document.createElement('tr');
          row.innerHTML = `
            <td>${file.name}</td>
            <td>
              <button onclick="downloadFile('${file.name}')">Download</button>
              <button class="delete-btn" onclick="deleteFile('${file.name}')">Delete</button>
            </td>
          `;
          tbody.appendChild(row);
        });
      } else {
        alert('Failed to fetch files');
      }
    }

    // Download a file
    function downloadFile(fileName) {
      window.location.href = `/download/${encodeURIComponent(fileName)}`;
    }

    // Delete a file
    async function deleteFile(fileName) {
      if (confirm(`Are you sure you want to delete "${fileName}"?`)) {
        const response = await fetch(`/delete/${encodeURIComponent(fileName)}`, { method: 'DELETE' });
        if (response.ok) {
          alert('File deleted successfully');
          fetchFiles(); // Refresh the file list
        } else {
          alert('Failed to delete file');
        }
      }
    }

    // Handle file upload
    document.getElementById('uploadForm').addEventListener('submit', async (e) => {
      e.preventDefault();
      const formData = new FormData(e.target);
      const response = await fetch('/upload', {
        method: 'POST',
        body: formData,
      });
      if (response.ok) {
        alert('File uploaded successfully');
        fetchFiles(); // Refresh the file list
      } else {
        alert('Failed to upload file');
      }
    });

    // Initial fetch of files
    fetchFiles();
  </script>
</body>
</html>
)rawliteral";

}  // namespace ftp_http_proxy
}  // namespace esphome

#endif  // FTP_WEB_H
