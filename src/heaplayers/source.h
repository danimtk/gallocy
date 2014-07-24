#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

#include <map>
#include <vector>

#include "myhashmap.h"
#include "spinlock.h"


#define ZONE_SZ   4096 * 16 * 2
#define MMAP_PROT PROT_READ|PROT_WRITE
#define MMAP_FLAG MAP_ANON|MAP_SHARED


class SimpleHeap {
  public:
    inline void* malloc(size_t sz) {
      void* mem = NULL;
      if (!zone) {
        if ((zone = mmap(NULL, ZONE_SZ, MMAP_PROT, MMAP_FLAG, -1, 0)) == MAP_FAILED) {
          fprintf(stderr, "MAP FAILED\n");
          return NULL;
        }
        next = (char*) zone;
        bytes_left = ZONE_SZ;
        //fprintf(stderr, "> OS gives %d bytes(s) at %p\n", ZONE_SZ, zone);
        //fprintf(stderr, "> Allocator(%p) has bytes_left=%lu\n", this, bytes_left);
      }
      if (bytes_left > sz) {
        mem  = (void*) next;
        next = next + sz;
        bytes_left -= sz;
        //fprintf(stderr, "> Allocated %zu byte(s) at %p\n", sz, mem);
        //fprintf(stderr, "> Allocator(%p) has bytes_left=%lu\n", this, bytes_left);
        return mem;
      }
      else {
        //fprintf(stderr, "> Exhausted virtual memory\n");
        //fprintf(stderr, "> Allocator(%p) has bytes_left=%lu\n", this, bytes_left);
        return NULL;
      }
    }

    inline void free(void* ptr) {
      // A `SimpleHeap` never free's memory once it is mapped. Note, could
      // unmap here using something like: munmap (reinterpret_cast<char
      // *>(ptr), getSize(ptr));
      munmap(reinterpret_cast<char*>(ptr), getSize(ptr));
      return;
    }

    inline void free(void* ptr, size_t sz) {
      return;
    }

    inline size_t getSize(void* ptr) {
      // This is broken, and always will be. Parent should never call this
      // method.
      return -1;
    }

  private:
    void* zone;
    char* next;
    unsigned long bytes_left;
};


class SourceHeap: public SimpleHeap {
  public:
    inline void * malloc (size_t sz) {
      void* ptr = SimpleHeap::malloc(sz);
      MyMapLock.lock();
      MyMap.set (ptr, sz);
      MyMapLock.unlock();
      //assert (reinterpret_cast<size_t>(ptr) % 4096 == 0);
      return const_cast<void *>(ptr);
    }

    inline size_t getSize (void * ptr) {
      MyMapLock.lock();
      size_t sz = MyMap.get(ptr);
      MyMapLock.unlock();
      return sz;
    }

    // WORKAROUND: apparent gcc bug.
    void free (void* ptr, size_t sz) {
      SimpleHeap::free (ptr, sz);
    }

    inline void free (void * ptr) {
      //assert (reinterpret_cast<size_t>(ptr) % 4096 == 0);
      MyMapLock.lock();
      size_t sz = MyMap.get(ptr);
      SimpleHeap::free(ptr, sz);
      MyMap.erase (ptr);
      MyMapLock.unlock();
    }

  private:

    class MyHeap :
      // FIX ME: 16 = size of ZoneHeap header.
      public HL::LockedHeap<HL::SpinLockType, HL::FreelistHeap<HL::ZoneHeap<SimpleHeap, 16384 - 16> > > {};

    typedef HL::MyHashMap<void *, size_t, MyHeap> mapType;

  protected:

    mapType MyMap;

    HL::SpinLockType MyMapLock;

};



class SingletonHeap {
  public:
    static SingletonHeap& getInstance() {
      static SingletonHeap instance;
      return instance;
    }

    static void* malloc(size_t sz) {
      return heap.malloc(sz);
    }

    static void free(void* ptr) {
      heap.free(ptr);
    }

  private:
    static SimpleHeap heap;

    // Need public for STL allocators...
    //
    //SingletonHeap() {};
    //SingletonHeap(SingletonHeap const&);
    //void operator=(SingletonHeap const&);

};

SimpleHeap singletonHeap;
SimpleHeap SingletonHeap::heap = singletonHeap;