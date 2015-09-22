#include <DB/IO/Operators.h>
#include <DB/Storages/StorageReplicatedMergeTree.h>
#include <DB/Storages/MergeTree/ReplicatedMergeTreeRestartingThread.h>
#include <DB/Storages/MergeTree/ReplicatedMergeTreeQuorumEntry.h>


namespace DB
{


/// Используется для проверки, выставили ли ноду is_active мы, или нет.
static String generateActiveNodeIdentifier()
{
	struct timespec times;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &times))
		throwFromErrno("Cannot clock_gettime.", ErrorCodes::CANNOT_CLOCK_GETTIME);
	return "pid: " + toString(getpid()) + ", random: " + toString(times.tv_nsec + times.tv_sec + getpid());
}


ReplicatedMergeTreeRestartingThread::ReplicatedMergeTreeRestartingThread(StorageReplicatedMergeTree & storage_)
	: storage(storage_),
	log(&Logger::get(storage.database_name + "." + storage.table_name + " (StorageReplicatedMergeTree, RestartingThread)")),
	active_node_identifier(generateActiveNodeIdentifier()),
	thread([this] { run(); })
{
}


void ReplicatedMergeTreeRestartingThread::run()
{
	constexpr auto retry_delay_ms = 10 * 1000;
	constexpr auto check_delay_ms = 60 * 1000;

	try
	{
		bool first_time = true;

		/// Запуск реплики при старте сервера/создании таблицы. Перезапуск реплики при истечении сессии с ZK.
		while (!need_stop)
		{
			if (first_time || storage.getZooKeeper()->expired())
			{
				if (first_time)
				{
					LOG_DEBUG(log, "Activating replica.");
				}
				else
				{
					LOG_WARNING(log, "ZooKeeper session has expired. Switching to a new session.");

					storage.is_readonly = true;
					partialShutdown();
				}

				while (true)
				{
					try
					{
						storage.setZooKeeper(storage.context.getZooKeeper());
					}
					catch (const zkutil::KeeperException & e)
					{
						/// Исключение при попытке zookeeper_init обычно бывает, если не работает DNS. Будем пытаться сделать это заново.
						tryLogCurrentException(__PRETTY_FUNCTION__);

						wakeup_event.tryWait(retry_delay_ms);
						continue;
					}

					if (!need_stop && !tryStartup())
					{
						wakeup_event.tryWait(retry_delay_ms);
						continue;
					}

					break;
				}

				storage.is_readonly = false;
				first_time = false;
			}

			wakeup_event.tryWait(check_delay_ms);
		}
	}
	catch (...)
	{
		tryLogCurrentException("StorageReplicatedMergeTree::restartingThread");
		LOG_ERROR(log, "Unexpected exception in restartingThread. The storage will be readonly until server restart.");
		goReadOnlyPermanently();
		LOG_DEBUG(log, "Restarting thread finished");
		return;
	}

	try
	{
		storage.endpoint_holder = nullptr;
		partialShutdown();
	}
	catch (...)
	{
		tryLogCurrentException(__PRETTY_FUNCTION__);
	}

	LOG_DEBUG(log, "Restarting thread finished");
}


bool ReplicatedMergeTreeRestartingThread::tryStartup()
{
	try
	{
		removeFailedQuorumParts();
		activateReplica();
		updateQuorumIfWeHavePart();

		storage.leader_election = new zkutil::LeaderElection(
			storage.zookeeper_path + "/leader_election",
			*storage.current_zookeeper,		/// current_zookeeper живёт в течение времени жизни leader_election,
											///  так как до изменения current_zookeeper, объект leader_election уничтожается в методе partialShutdown.
			[this] { storage.becomeLeader(); },
			storage.replica_name);

		/// Все, что выше, может бросить KeeperException, если что-то не так с ZK.
		/// Все, что ниже, не должно бросать исключений.

		storage.shutdown_called = false;
		storage.shutdown_event.reset();

		storage.merger.uncancelAll();
		if (storage.unreplicated_merger)
			storage.unreplicated_merger->uncancelAll();

		storage.queue_updating_thread = std::thread(&StorageReplicatedMergeTree::queueUpdatingThread, &storage);
		storage.cleanup_thread.reset(new ReplicatedMergeTreeCleanupThread(storage));
		storage.alter_thread = std::thread(&StorageReplicatedMergeTree::alterThread, &storage);
		storage.part_check_thread = std::thread(&StorageReplicatedMergeTree::partCheckThread, &storage);
		storage.queue_task_handle = storage.context.getBackgroundPool().addTask(
			std::bind(&StorageReplicatedMergeTree::queueTask, &storage, std::placeholders::_1));
		storage.queue_task_handle->wake();

		return true;
	}
	catch (...)
	{
		storage.replica_is_active_node 	= nullptr;
		storage.leader_election 		= nullptr;

		try
		{
			throw;
		}
		catch (const zkutil::KeeperException & e)
		{
			LOG_ERROR(log, "Couldn't start replication: " << e.what() << ", " << e.displayText() << ", stack trace:\n" << e.getStackTrace().toString());
			return false;
		}
		catch (const Exception & e)
		{
			if (e.code() != ErrorCodes::REPLICA_IS_ALREADY_ACTIVE)
				throw;

			LOG_ERROR(log, "Couldn't start replication: " << e.what() << ", " << e.displayText() << ", stack trace:\n" << e.getStackTrace().toString());
			return false;
		}
	}
}


void ReplicatedMergeTreeRestartingThread::removeFailedQuorumParts()
{
	auto zookeeper = storage.getZooKeeper();

	Strings failed_parts;
	if (!zookeeper->tryGetChildren(storage.zookeeper_path + "/quorum/failed_parts", failed_parts))
		return;

	for (auto part_name : failed_parts)
	{
		auto part = storage.data.getPartIfExists(part_name);
		if (part)
		{
			LOG_DEBUG(log, "Found part " << part_name << " with failed quorum. Moving to detached. This shouldn't happen often.");

			zkutil::Ops ops;
			storage.removePartFromZooKeeper(part_name, ops);
			auto code = zookeeper->tryMulti(ops);
			if (code == ZNONODE)
				LOG_WARNING(log, "Part " << part_name << " with failed quorum is not in ZooKeeper. This shouldn't happen often.");

			storage.data.renameAndDetachPart(part, "noquorum");
		}
	}
}


void ReplicatedMergeTreeRestartingThread::updateQuorumIfWeHavePart()
{
	auto zookeeper = storage.getZooKeeper();

	String quorum_str;
	if (zookeeper->tryGet(storage.zookeeper_path + "/quorum/status", quorum_str))
	{
		ReplicatedMergeTreeQuorumEntry quorum_entry;
		quorum_entry.fromString(quorum_str);

		if (!quorum_entry.replicas.count(storage.replica_name)
			&& zookeeper->exists(storage.replica_path + "/parts/" + quorum_entry.part_name))
		{
			LOG_WARNING(log, "We have part " << quorum_entry.part_name
				<< " but we is not in quorum. Updating quorum. This shouldn't happen often.");
			storage.updateQuorum(quorum_entry.part_name);
		}
	}
}


void ReplicatedMergeTreeRestartingThread::activateReplica()
{
	auto host_port = storage.context.getInterserverIOAddress();
	auto zookeeper = storage.getZooKeeper();

	std::string address;
	{
		WriteBufferFromString address_buf(address);
		address_buf
			<< "host: " << host_port.first << '\n'
			<< "port: " << host_port.second << '\n';
	}

	String is_active_path = storage.replica_path + "/is_active";

	/** Если нода отмечена как активная, но отметка сделана в этом же экземпляре, удалим ее.
	  * Такое возможно только при истечении сессии в ZooKeeper.
	  */
	String data;
	Stat stat;
	bool has_is_active = zookeeper->tryGet(is_active_path, data, &stat);
	if (has_is_active && data == active_node_identifier)
	{
		auto code = zookeeper->tryRemove(is_active_path, stat.version);

		if (code == ZBADVERSION)
			throw Exception("Another instance of replica " + storage.replica_path + " was created just now."
				" You shouldn't run multiple instances of same replica. You need to check configuration files.",
				ErrorCodes::REPLICA_IS_ALREADY_ACTIVE);

		if (code != ZOK && code != ZNONODE)
			throw zkutil::KeeperException(code, is_active_path);
	}

	/// Одновременно объявим, что эта реплика активна, и обновим хост.
	zkutil::Ops ops;
	ops.push_back(new zkutil::Op::Create(is_active_path,
		active_node_identifier, zookeeper->getDefaultACL(), zkutil::CreateMode::Ephemeral));
	ops.push_back(new zkutil::Op::SetData(storage.replica_path + "/host", address, -1));

	try
	{
		zookeeper->multi(ops);
	}
	catch (const zkutil::KeeperException & e)
	{
		if (e.code == ZNODEEXISTS)
			throw Exception("Replica " + storage.replica_path + " appears to be already active. If you're sure it's not, "
				"try again in a minute or remove znode " + storage.replica_path + "/is_active manually", ErrorCodes::REPLICA_IS_ALREADY_ACTIVE);

		throw;
	}

	/// current_zookeeper живёт в течение времени жизни replica_is_active_node,
	///  так как до изменения current_zookeeper, объект replica_is_active_node уничтожается в методе partialShutdown.
	storage.replica_is_active_node = zkutil::EphemeralNodeHolder::existing(is_active_path, *storage.current_zookeeper);
}


void ReplicatedMergeTreeRestartingThread::partialShutdown()
{
	storage.leader_election = nullptr;
	storage.shutdown_called = true;
	storage.shutdown_event.set();
	storage.merge_selecting_event.set();
	storage.queue_updating_event->set();
	storage.alter_thread_event->set();
	storage.alter_query_event->set();
	storage.parts_to_check_event.set();
	storage.replica_is_active_node = nullptr;

	storage.merger.cancelAll();
	if (storage.unreplicated_merger)
		storage.unreplicated_merger->cancelAll();

	LOG_TRACE(log, "Waiting for threads to finish");
	if (storage.is_leader_node)
	{
		storage.is_leader_node = false;
		if (storage.merge_selecting_thread.joinable())
			storage.merge_selecting_thread.join();
	}
	if (storage.queue_updating_thread.joinable())
		storage.queue_updating_thread.join();

	storage.cleanup_thread.reset();

	if (storage.alter_thread.joinable())
		storage.alter_thread.join();
	if (storage.part_check_thread.joinable())
		storage.part_check_thread.join();
	if (storage.queue_task_handle)
		storage.context.getBackgroundPool().removeTask(storage.queue_task_handle);
	storage.queue_task_handle.reset();
	LOG_TRACE(log, "Threads finished");
}


void ReplicatedMergeTreeRestartingThread::goReadOnlyPermanently()
{
	LOG_INFO(log, "Going to readonly mode");

	storage.is_readonly = true;
	stop();

	partialShutdown();
}


}
