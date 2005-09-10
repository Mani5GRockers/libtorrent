// libTorrent - BitTorrent library
// Copyright (C) 2005, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#ifndef LIBTORRENT_DATA_CHUNK_LIST_H
#define LIBTORRENT_DATA_CHUNK_LIST_H

#include <vector>
#include <sigc++/slot.h>

#include "chunk_handle.h"

namespace torrent {

class ChunkList : private std::vector<ChunkListNode> {
public:
  typedef uint32_t                            size_type;
  typedef std::vector<ChunkListNode>          Base;
  typedef std::vector<ChunkListNode*>         Queue;
  typedef sigc::slot2<Chunk*, uint32_t, bool> SlotCreateChunk;

  using Base::value_type;
  using Base::reference;

  using Base::iterator;
  using Base::reverse_iterator;
  using Base::size;
  using Base::empty;

  ~ChunkList() { clear(); }

  bool                has_chunk(size_type index, int prot) const;

  void                resize(size_type s);
  void                clear();

  ChunkHandle         get(size_type index, bool writable);
  void                release(ChunkHandle handle);

  // Possibly have multiple version, some that do syncing of
  // sequential chunks only etc. Pretty much depends on the time of
  // dereferencing etc.
  void                sync_all();
  void                sync_periodic();

  void                slot_create_chunk(SlotCreateChunk s) { m_slotCreateChunk = s; }

private:
  inline bool         is_queued(ChunkListNode* node);

  static inline void  sync_chunk(ChunkListNode* node);

  SlotCreateChunk     m_slotCreateChunk;
  Queue               m_queue;
};

}

#endif