#ifndef __OB_IMPORT_PRODUCER_H__
#define  __OB_IMPORT_PRODUCER_H__

#include "db_queue.h"
#include "file_reader.h"

class ImportProducer : public QueueProducer<RecordBlock> {
  public:
    ImportProducer(FileReader &reader, const RecordDelima &delima, const RecordDelima &rec_delima) 
      : reader_(reader), delima_(delima), rec_delima_(rec_delima) { }

    int init();

    int produce(RecordBlock &obj);
  private:
    FileReader &reader_;

    RecordDelima delima_;
    RecordDelima rec_delima_;
};

#endif
