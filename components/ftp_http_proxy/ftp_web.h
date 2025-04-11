#pragma once

const char WEB_INTERFACE[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Gestion FTP</title>
  <style>
    body { font-family: Arial; max-width: 800px; margin: 0 auto; padding: 20px; }
    table { width: 100%; border-collapse: collapse; margin: 20px 0; }
    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
    th { background-color: #f2f2f2; }
    button { padding: 5px 10px; margin: 2px; cursor: pointer; }
    .upload-area { border: 2px dashed #ccc; padding: 20px; text-align: center; margin: 20px 0; }
  </style>
</head>
<body>
  <h1>Gestionnaire FTP</h1>
  <div class="upload-area">
    <input type="file" id="fileInput" multiple>
    <button onclick="uploadFiles()">Upload</button>
  </div>
  <table>
    <thead><tr><th>Nom</th><th>Taille</th><th>Actions</th></tr></thead>
    <tbody id="fileList"></tbody>
  </table>
  <script>
    async function fetchFiles() {
      const response = await fetch('/list');
      const files = await response.json();
      const fileList = document.getElementById('fileList');
      fileList.innerHTML = '';
      files.forEach(file => {
        const row = document.createElement('tr');
        row.innerHTML = `
          <td>${file.name}</td>
          <td>${file.size} bytes</td>
          <td>
            <button onclick="downloadFile('${file.name}')">Télécharger</button>
            <button onclick="deleteFile('${file.name}')">Supprimer</button>
          </td>
        `;
        fileList.appendChild(row);
      });
    }

    function downloadFile(filename) {
      window.open('/' + filename, '_blank');
    }

    async function deleteFile(filename) {
      if (confirm('Supprimer ' + filename + '?')) {
        await fetch('/' + filename, { method: 'DELETE' });
        fetchFiles();
      }
    }

    async function uploadFiles() {
      const input = document.getElementById('fileInput');
      const formData = new FormData();
      for (const file of input.files) {
        formData.append('file', file);
      }
      await fetch('/upload', { method: 'POST', body: formData });
      fetchFiles();
    }

    fetchFiles();
  </script>
</body>
</html>
)rawliteral";
