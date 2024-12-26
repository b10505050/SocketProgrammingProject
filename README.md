# SocketProgrammingProject
這是一個以 C/C++ 寫成的簡易 SocketProgramming 專案，能透過 OpenSSL 與 OpenCV 實現下列功能：

	1. 使用者註冊 / 登入
	2. OpenSLL 加密
	3. 線上用戶查詢
	4. 即時訊息傳送 (SEND)
	5. 檔案上傳 / 下載 (SEND_FILE / RECEIVE_FILE)
	6. 檔案清單查看 (LIST_FILES)
	7. 串流影片 (STREAM_VIDEO) – Client 讀取影片並以 JPEG 格式壓縮連續傳給 Server，Server 端以 OpenCV 顯示視訊畫面。

環境需求
	1. 作業系統：Linux / macOS (或能執行 POSIX socket + OpenSSL + OpenCV 的環境)
	2. OpenSSL
	3. 安裝開發版函式庫（例如 libssl-dev, libcrypto-dev）
	4. OpenCV
	5. 安裝包含核心、影像編解碼、HighGUI、VideoIO 等套件。
	6. PThread (伺服器端使用 pthread 進行多執行緒處理)
	7. C/C++ 編譯器
	8. g++ / clang++ 等，支援 C++11 (或更新)

Project Struct : 
	.
	├─ server.c                // 伺服器端程式
	├─ client.c                // 客戶端程式
	├─ server.crt              // 伺服器 SSL 憑證
	├─ server.key              // 伺服器 SSL 私鑰
	├─ user_db                 // 使用者帳號密碼資料庫
	├─ store/                  // 上傳檔案將存放於此目錄
	├─ 1.txt  		    // Testing file for Transfering
	├─ 123.mkv		    // Testing file for streaming
	└─ README                  // 此說明文件

What & How can it do : 
       -Main Menu

	1. Register：註冊新帳號（會寫入 user_db）
	2. Login：登入已存在的帳號
	3. Exit：離開程式
	
      -Login 後的次選單 (以程式列印為準) : 

	1. View online users：查詢當前在線使用者列表
	2. Retrieve messages：取得「別人傳給你的離線訊息」
	3. Send message：送訊息給其他在線使用者
	4. Logout：登出並回到主選單
	5. Send file：上傳本地檔案到 Server store/ 資料夾
	6. List file：查看 store/ 裏有哪些檔案可下載
	7. Receive file：下載指定檔案
	8. Send video file：串流影片（Client 端讀取影片檔、JPEG 壓縮後連續傳給 Server，Server 用 OpenCV 即時顯示）
		- 串流影片注意事項
			Client：
				1. 選擇「8. Send video file」，輸入影片路徑（如 myvideo.mp4）

				2. 影片讀取完後，程式會發送 frame_size=0 表示結束

				3. 不要直接強制關閉 Client，否則 Server 端會出現 SSL EOF 錯誤

			Server：

				1. 收到 "STREAM_VIDEO" 指令後，進入 handle_video_stream()

				2. 每個 frame 會在 OpenCV 視窗顯示

				3. 若按下 ESC 或收到 frame_size=0 時結束串流

				4. 必須有正確安裝 OpenCV，否則可能顯示不了畫面

		- 檔案上傳 / 下載
			上傳 (SEND_FILE)
				1. Client 選單 [5] Send file -> 輸入本地檔案名稱 -> 傳給 Server

				2. Server 會將該檔案存至 store/<filename>
			下載 (RECEIVE_FILE)
				1. Client 選單 [7] Receive file -> 輸入要下載的檔名 -> Server 從 store/ 讀取並傳回

				2. Client 預設若有同名檔，會自動在檔名後加 _1, _2, ... 以防覆蓋
		- 常見問題
				1. 為何出現 ssl3_get_record:http request 錯誤？
					可能是用瀏覽器或非 SSL 程式連線到此 port。此程式只支援自定義的 SSL 協定，不是 HTTP/HTTPS 伺服器。
				2. 為何 Server 報 unexpected EOF while reading？
					可能是 Client 在傳檔案或串流途中強制關閉程式，導致連線被動結束。

				3. 為何 OpenCV 視窗沒顯示或出錯？
					請確保安裝了支援 GUI 顯示的 OpenCV (build with -D WITH_QT=ON 或 -D WITH_GTK=ON 等)
					遠端 SSH 可能需要 X11 forwarding 或使用 -DOPENCV_ENABLE_NONFREE=ON 之類設定。

// ====================================== Instruction for Compiling / Executing =================================================// 
Before Compile : 
	sudo apt update
	sudo apt install libopencv-dev

Check OpenCV : 
	pkg-config --modversion opencv4
	ls /usr/include/opencv4/opencv2
	pkg-config --cflags opencv4
	pkg-config --libs opencv4

Generate OpenSLL : 
	openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout server.key -out server.crt

Compile : 
	g++ server.c -o server $(pkg-config --cflags --libs opencv4) -lssl -lcrypto
	g++ client.c -o client $(pkg-config --cflags --libs opencv4) -lssl -lcrypto

Execute : 
	./server
	./client
