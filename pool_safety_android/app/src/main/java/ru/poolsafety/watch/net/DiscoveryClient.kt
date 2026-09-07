package ru.poolsafety.watch.net

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface

// ---------------------------------------------------------------------------
//  Поиск компьютера с программой в локальной сети.
//
//  ЗАЧЕМ ЭТО ВООБЩЕ. Чтобы у оператора не было повода лезть в настройки сети.
//  Телефон кричит в свою сеть «кто здесь PoolSafety?», компьютер отвечает —
//  и адрес вписывать не нужно.
//
//  НО РУЧНОЙ ВВОД ОСТАЁТСЯ ОБЯЗАТЕЛЬНЫМ. Широковещательный запрос доходит не
//  всегда: гостевые сети с разделением клиентов его режут, а телефон может
//  оказаться в другой подсети. Поиск — это удобство, а не единственный путь.
// ---------------------------------------------------------------------------

data class FoundServer(val host: String, val port: Int, val panels: Int)

object DiscoveryClient {

    private const val TAG = "PoolSafety"

    /// Спросить сеть и подождать ответов.
    ///
    /// [timeoutMs] — сколько всего слушаем. Возвращает всех, кто отозвался:
    /// компьютеров с программой может быть больше одного, и выбирать должен
    /// человек, а не мы за него.
    suspend fun search(timeoutMs: Int = 2500): List<FoundServer> =
        withContext(Dispatchers.IO) {
            val found = LinkedHashMap<String, FoundServer>()

            runCatching {
                DatagramSocket().use { socket ->
                    socket.broadcast = true
                    socket.soTimeout = 300

                    val request = Protocol.DISCOVERY_REQUEST.toByteArray()
                    for (address in broadcastAddresses()) {
                        runCatching {
                            socket.send(
                                DatagramPacket(
                                    request, request.size, address, Protocol.DISCOVERY_PORT
                                )
                            )
                        }
                    }

                    val buffer = ByteArray(2048)
                    val deadline = System.currentTimeMillis() + timeoutMs
                    while (System.currentTimeMillis() < deadline) {
                        val packet = DatagramPacket(buffer, buffer.size)
                        val received = runCatching { socket.receive(packet); true }
                            .getOrDefault(false)
                        if (!received) continue

                        val text = String(packet.data, 0, packet.length)
                        val json = runCatching { JSONObject(text) }.getOrNull() ?: continue
                        if (json.optString("app") != "PoolSafety") continue

                        val host = packet.address.hostAddress ?: continue
                        found[host] = FoundServer(
                            host = host,
                            port = json.optInt("port", Protocol.DEFAULT_PORT),
                            panels = json.optInt("panels", 0)
                        )
                    }
                }
            }.onFailure { Log.w(TAG, "поиск компьютера не удался: ${it.message}") }

            found.values.toList()
        }

    /// Куда кричать. Общий адрес 255.255.255.255 доходит не в каждой сети,
    /// поэтому к нему добавляются широковещательные адреса своих сетей.
    private fun broadcastAddresses(): List<InetAddress> {
        val result = ArrayList<InetAddress>()
        result += runCatching { InetAddress.getByName("255.255.255.255") }
            .getOrNull() ?: return result

        runCatching {
            for (nic in NetworkInterface.getNetworkInterfaces()) {
                if (!nic.isUp || nic.isLoopback) continue
                for (address in nic.interfaceAddresses) {
                    address.broadcast?.let { result += it }
                }
            }
        }
        return result.distinct()
    }
}
